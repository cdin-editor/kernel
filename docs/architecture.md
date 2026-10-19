# Architecture and ownership

`cdin-kernel` is a C11 library divided by data ownership and responsibility. The code intentionally exposes one public header and keeps implementation declarations in `src/internal.h`.

## Component map

```text
Application / editor backend
          |
          v
  include/cdin/kernel.h     Public C ABI and opaque handles
          |
          v
  src/api/*                 Validation boundary, locking, output checks, callbacks
          |
          +--> src/doc/*     Document lifecycle, edits, reload, snapshots
          +--> src/txn/*     Transaction lifecycle and tentative text
          +--> src/history/* Undo units, redo tail, clean marker, eviction
          +--> src/search/*  Literal/byte scanning and case-fold preparation
          +--> src/text/*    Byte/UTF-8 positions, lines, grapheme boundaries
          +--> src/notif/*   Per-subscriber bounded rings and wakeup list
          +--> src/core/*    Ref-counted text blocks and generation handles
          +--> src/util/*    Allocation discipline, UTF-8 decoder, Unicode tables
```

`src/kernel.c` owns version/features queries, kernel construction/destruction, shutdown, sequence tracking, and lock/log helpers. Public wrappers are grouped by header concern under `src/api/`; policy is implemented in the underlying subsystem files.

## Ownership rules

### Kernel owns state; callers own buffers

All document data, history records, snapshots, transactions, handle tables, notification rings, and search pattern staging allocations made by the kernel go through the configured `cdin_allocator`. The caller owns request structs, input byte buffers, output arrays, and any external synchronization context. The kernel copies bytes that it needs to keep after the API call.

### Text blocks use reference counting and copy-on-write

A document holds the current text block. A snapshot holds a reference to the block captured at acquisition. A search pins a block for the duration of its scan. Mutation uses copy-on-write whenever the current block is shared, so a snapshot or an in-progress search does not observe a changing byte buffer.

This design gives snapshots stable content/version pairs without forcing every read to copy the document. The snapshot handle itself must still be explicitly released.

### Handles are generation-checked

Documents, transactions, snapshots, and subscriptions use typed handle pools. A handle is an opaque 64-bit value; pool generations allow stale handles to be rejected instead of accidentally naming a new object after slot reuse. Callers must nevertheless respect each handle's lifetime and kernel association.

## Mutation lifecycle

Document mutation is structured to preserve failure atomicity:

1. Validate struct shape, flags, handles, version, positions, and resource limits.
2. Accept the operation and advance the accepted-mutation sequence only after early validation/version checks pass.
3. Allocate/prep the replacement/history/notification state before publishing content.
4. Apply the edit using operations that are expected not to fail, update version/history/clean marker, and enqueue notification events.
5. Release the kernel lock, then run any wakeup callbacks collected for subscribers.

A failed content mutation must not expose half of the requested edit or consume a content version. Tests in `tests/test_p1.c` exercise failure atomicity and strict allocator accounting.

## Transactions

A transaction initially shares the document's text block and creates a private working version on the first copy-on-write edit. While the transaction is active, other mutations of that document return `CDIN_BUSY`; ordinary document reads keep seeing committed state. Commit prepares the undo unit and then publishes the transaction's working block as a single version change. Abort discards the tentative block. A failed commit rolls back and consumes the transaction handle.

## Locking and callback boundaries

All public APIs except destruction are written around `k_lock`/`k_unlock`. Those helpers invoke the optional lock vtable supplied in `cdin_kernel_config`; the kernel does not instantiate a mutex by itself. With no lock pair, callers must serialize calls to that kernel on one thread. With a lock pair, callers must provide a real consistent lock/unlock pair and keep its context alive.

Search has a deliberate unlocked scan window: while the source block is pinned, the scan can run without holding the kernel lock. After scanning, the block is released under the lock. Notifications use a wake list so wakeup callbacks are not invoked while the core lock is held. Callbacks should signal/enqueue work, not run arbitrary application tasks inline.

`cdin_kernel_destroy` remains an exclusive/owner-thread operation; stop all worker threads first. The optional log sink may run during teardown and must remain valid until destroy returns.

## Notifications and recovery

Each subscriber has a bounded ring. The producer never waits for a consumer. When the ring overflows, queued incremental events are replaced with a `CDIN_NOTIF_RESYNC` event carrying the version that the consumer should re-read. If a consumer sees `RESYNC` or `edits_truncated`, it must not reconstruct state by replaying only the stored edit descriptors.

## Exact bytes and Unicode views

The underlying text abstraction is byte-based. UTF-8 decoding classifies valid multi-byte sequences and raw bytes without mutating the input. Position functions expose code points and extended grapheme clusters on top of that representation; raw bytes remain independently addressable. Line indexing recognizes LF, CRLF, and CR, preserving the original terminators.

Unicode tables are generated/embedded under `src/util/`; `tools/gen_unicode.py` documents the generator entry point. The runtime version query is the public way to discover the Unicode table version in the built library.

## Foreign-language integration

Because the stable kernel API includes C structs, function pointers, explicit memory callbacks, and binary handles, a language binding must match native ABI layout exactly. The examples bridge in `examples/ffi/` intentionally trades coverage for simpler signatures. It wraps only a small set of operations and uses the default C allocator; it is an example adapter, not a second compatibility promise. See [language bindings](language-bindings.md).
