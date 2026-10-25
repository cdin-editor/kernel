# C ABI reference

This document is the precise reference for the public C ABI of `cdin-kernel`.
It describes types, structs, and the contract each function must satisfy.
The behavioral tutorial is the [user guide](user-guide.md); this document
states contracts, not workflows.

Authoritative source: `include/cdin/kernel.h`. ABI version at the time of
writing: **1.0.0** (`CDIN_KERNEL_ABI_MAJOR == 1`, minor 0). Unicode data:
**15.1.0**. Where this document and the header disagree, the header wins.

## 1. Global shape of the ABI

- Every public function returns `cdin_status` and communicates results
  exclusively through caller-allocated output parameters.
- Every public struct with a `struct_size` field demands that value be set by
  the caller on input (and, for output structs, set to `sizeof` before the
  call); the library never inherits stale values.
- The library never transfers ownership of an internal buffer; all data that
  crosses the boundary is copied in or out under caller-owned memory.
- No public function performs blocking I/O, spawns threads or processes, or
  uses process-global state.
- No public function allocates through anything but the `cdin_allocator`
  supplied at kernel creation (verbatim: no `malloc`/`free` calls anywhere in
  the library).
- The ABI is C11. C++ applications may include the header: it is wrapped in
  `extern "C"`. C++ ABI compatibility is not claimed beyond compiling; pass
  and receive structs by value in C-facing wrappers.

## 2. Types

### 2.1 Version and feature macros

| Macro | Value | Meaning |
|---|---|---|
| `CDIN_KERNEL_ABI_MAJOR` | `1u` | ABI major; a match is required at `cdin_kernel_create`. |
| `CDIN_KERNEL_ABI_MINOR` | `0u` | ABI minor; grows for compatible additions. |
| `CDIN_ANY_VERSION` | `UINT64_MAX` | Disables version checks; the value can never be a real document version. |

The runtime queries (all return `cdin_status`; set output only on `CDIN_OK`):

```c
cdin_status cdin_abi_version(uint32_t *major, uint32_t *minor, uint32_t *patch);
cdin_status cdin_features(uint64_t *mask);
cdin_status cdin_unicode_version(uint32_t *major, uint32_t *minor, uint32_t *patch);
const char *cdin_status_name(cdin_status status);
```

`cdin_status_name` accepts any value, including unknown ones, and always
returns a process-lifetime static string.

`cdin_features` returns a 64-bit bit-set (`uint64_t`):

| Bit | Feature flag | Meaning | Set in 1.0.0? |
|---|---|---|---|
| 0 | `CDIN_FEAT_BATCH` | batch replacement | yes |
| 1 | `CDIN_FEAT_TRANSACTIONS` | transactions | yes |
| 2 | `CDIN_FEAT_SNAPSHOTS` | snapshots | yes |
| 3 | `CDIN_FEAT_NOTIFICATIONS` | subscriptions | yes |
| 4 | `CDIN_FEAT_SEARCH_LITERAL` | literal search | yes |
| 5 | `CDIN_FEAT_SEARCH_BYTES` | byte search | yes |
| 6 | `CDIN_FEAT_SEARCH_REGEX` | regex search | **no** |
| 7 | `CDIN_FEAT_GRAPHEME` | grapheme boundaries | yes |
| 8 | `CDIN_FEAT_CASEFOLD_SIMPLE` | simple case folding | yes |
| 9 | `CDIN_FEAT_LOCKING` | lock vtable | yes |

Undefined bits are reserved. Query with

```c
uint64_t mask = 0;
cdin_features(&mask);
if (mask & CDIN_FEAT_BATCH) { /* call the batch API */ }
```

before constructing calls that need a feature.

### 2.2 Status codes

`cdin_status` is `typedef uint32_t cdin_status` with anonymous enum constants.
The underlying type is fixed so its width is stable across compilers; the
numeric values are part of ABI major 1 and must not be renumbered.

| Value | Name | Contract |
|---|---|---|
| 0 | `CDIN_OK` | Call succeeded; only then are output parameters valid. |
| 1 | `CDIN_INVALID_ARGUMENT` | Shape of the request is wrong: NULL, bad size, bad flag, bad range or ordering before length checks. |
| 2 | `CDIN_INVALID_HANDLE` | Handle not live: closed, released, committed, stale generation, or foreign. |
| 3 | `CDIN_STALE_VERSION` | Version check failed; see `cdin_error_details`. |
| 4 | `CDIN_OUT_OF_RANGE` | A value exceeded its bound; the bound is not `struct_size` (for that see `CDIN_UNSUPPORTED` or `CDIN_INVALID_ARGUMENT`). |
| 5 | `CDIN_INVALID_POSITION` | Position inside a multi-byte sequence. |
| 6 | `CDIN_INVALID_STATE` | Legal call, illegal state: second transaction begin, undo with empty history, redo with nothing to redo. |
| 7 | `CDIN_BUSY` | Object in use: document edit blocked by an active transaction. |
| 8 | `CDIN_LIMIT_EXCEEDED` | A documented limit was hit; `details.limit_id` says which. |
| 9 | `CDIN_NO_MEMORY` | The allocator refused; nothing was changed except possibly a consumed sequence number (see 2.5). |
| 10 | `CDIN_PATTERN_INVALID` | Search pattern is not valid UTF-8 (literal mode only). |
| 11 | `CDIN_CANCELLED` | Search aborted because the cancel flag turned non-zero. |
| 12 | `CDIN_SHUTDOWN` | Kernel has been shut down. |
| 13 | `CDIN_UNSUPPORTED` | Declared but unimplemented path in this ABI: regex, reserved `flags` set with a documented meaning, struct extension with non-zero unknown bytes. |
| 14 | `CDIN_BUFFER_TOO_SMALL` | Out-buffer too small; a size is reported. |
| 15 | `CDIN_VERSION_MISMATCH` | `cfg.abi_major` did not equal `CDIN_KERNEL_ABI_MAJOR`. |
| 16 | `CDIN_INTERNAL` | Kernel invariant violation; the state is undefined; report it as a library bug. |

Only `CDIN_OK` is success. Any other status leaves the document unchanged.

### 2.3 Error details

```c
typedef struct cdin_error_details {
    uint32_t struct_size;
    uint32_t limit_id;
    uint64_t expected;
    uint64_t current;
    uint64_t requested;
    uint64_t length;
    uint64_t required;
} cdin_error_details;
```

| Status | Fields set |
|---|---|
| `CDIN_LIMIT_EXCEEDED` | `limit_id` |
| `CDIN_STALE_VERSION` | `expected` (the requested), `current` |
| `CDIN_OUT_OF_RANGE` | `requested` (offending value), `length` (bound) |
| `CDIN_INVALID_POSITION` | `requested`, `length` |
| `CDIN_BUFFER_TOO_SMALL` | `required` (for history blobs only; `text_read`/`serialize`/`txn_read` instead set `*out_len`) |

Limit IDs:

| Value | Name | checks |
|---|---|---|
| 0 | `CDIN_LIMIT_NONE` | unused placeholder |
| 1 | `CDIN_LIMIT_DOCUMENTS` | live documents per kernel |
| 2 | `CDIN_LIMIT_DOCUMENT_SIZE` | document size in bytes |
| 3 | `CDIN_LIMIT_EDIT_SIZE` | bytes inserted by one edit (or one batch total) |
| 4 | `CDIN_LIMIT_UNDO_MEMORY` | history memory per document |
| 5 | `CDIN_LIMIT_BLOB_SIZE` | caller blob size |
| 6 | `CDIN_LIMIT_SNAPSHOTS` | snapshots per document |
| 7 | `CDIN_LIMIT_SUBSCRIBERS` | subscribers per document |
| 8 | `CDIN_LIMIT_SEARCH_STEPS` | search step budget |
| 9 | `CDIN_LIMIT_TXN_MEMORY` | tentative transaction memory |
| 10 | `CDIN_LIMIT_BATCH_EDITS` | number of edits in one batch |
| 11 | `CDIN_LIMIT_VERSION` | version counter paid out and can go no further |
| 12 | `CDIN_LIMIT_SEQUENCE` | sequence counter exhausted |

### 2.4 Shutdown modes

`cdin_shutdown_mode`:

| Value | Name | Behavior |
|---|---|---|
| 0 | `CDIN_SHUTDOWN_GRACEFUL` | Behaves identically to abort in 1.0.0 (all work is synchronous). |
| 1 | `CDIN_SHUTDOWN_ABORT` | Same. Both stop new mutations and abort open transactions. |

### 2.5 Sequentiality

- Every accepted request consumes the next value of a monotonic kernel-wide
  sequence; `accepted_seq` in an `cdin_edit_result` records it.
- A request that fails a version check or any validation stage consumes
  nothing. A request that passes validation and the version check but fails
  while allocating memory consumes its number and produces no document change.
- `accepted_seq == 0` therefore means "not accepted"; a nonzero value says
  "accepted, order fixed by the sequence, application completion undefined
  until the status is known".
- Sequence numbers have no meaning across kernel restarts and are not
  timestamps.

## 3. Handles and sources

| Type | Issued by | Invalidated by |
|---|---|---|
| `cdin_doc doc` | `cdin_doc_create` | `cdin_doc_close`, `cdin_kernel_destroy` |
| `cdin_txn t` | `cdin_txn_begin` | `cdin_txn_commit`, `cdin_txn_abort`, doc close, kernel shutdown/destroy |
| `cdin_snapshot s` | `cdin_snapshot_acquire` | `cdin_snapshot_release`, kernel destroy |
| `cdin_notif n` | `cdin_notif_subscribe` | `cdin_notif_unsubscribe`, kernel destroy |

All handles are 64-bit; 0 is never valid. A handle is opaque. Handle values
are not comparable across object types: doc/txn/snapshot/notif namespaces are
independent, and reuse after invalidation yields a different generation-tagged
value with the same slot index, guaranteeing old handles never work again.

`cdin_source` names a document or snapshot for reading:

```c
typedef struct cdin_source {
    uint64_t doc;      /* exactly one field non-zero, the other zero */
    uint64_t snapshot;
} cdin_source;

static inline cdin_source cdin_src_doc(cdin_doc doc);        /* {doc, 0} */
static inline cdin_source cdin_src_snap(cdin_snapshot snap); /* {0, snap} */
```

Both fields zero, or both non-zero, is `CDIN_INVALID_ARGUMENT`.

`cdin_version` is `uint64_t`. Version 0 is a freshly created document; version
counters only increase and never wrap.

`cdin_edit_desc` describes a change in the coordinates of the state right
before a change:

```c
typedef struct cdin_edit_desc {
    uint64_t start;      /* byte offset of the replacement range */
    uint64_t removed;    /* bytes removed */
    uint64_t inserted;   /* bytes inserted */
} cdin_edit_desc;
```

## 4. `struct_size` protocol

For an input struct `T` with `struct_size`, the caller sets

```c
request.struct_size = (uint32_t)sizeof request;
```

The kernel accepts the struct when:

- `declared >= sizeof(struct known_to_this_library)`, and
- every byte beyond the known size is zero.

Otherwise it returns `CDIN_INVALID_ARGUMENT` (smaller declared size) or
`CDIN_UNSUPPORTED` (larger with non-zero unknown bytes). Corollary: a program
built against a *newer* header still runs against a library that only knows a
smaller struct, provided it leaves its extra fields zero (it may not use
features the older library lacks). A program built against an *older* header
fails against a newer library with `CDIN_INVALID_ARGUMENT` until it is
recompiled; the error is explicit, never a silent misread of a struct.

Output structs (`cdin_edit_result`, `cdin_search_result`, `cdin_eol_info`)
receive `details`/field values only after `CDIN_OK`. `struct_size` on those is
the caller's input as well.

`cdin_kernel_config` is special only because its two sub-tables
(`cdin_allocator`, `cdin_lock_vtable`, `cdin_log_sink`) are embedded by value
and require *exactly* `sizeof` their type (see 5), because they cannot carry a
forward-compatible tail in ABI 1.

## 5. Callback contracts

### 5.1 `cdin_allocator`

```c
typedef struct cdin_allocator {
    uint32_t struct_size;
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} cdin_allocator;
```

- Required at kernel creation; `struct_size` must equal `sizeof(cdin_allocator)`.
- `alloc(ctx, size)` must return a pointer to at least `size` writable bytes
  (alignment suitably general for any standard type), or NULL on failure.
  `size` is never 0.
- `realloc(...)` must return the relocated block or NULL. On failure the old
  block must remain valid and unchanged; the library then frees it. The
  library never passes NULL as `ptr`, and never shrinks to zero.
- `free(ctx, ptr, size)` must release the block. `size` is exactly the size
  the library allocated (the original or the last `realloc` request); if the
  allocator tracks something other than that, it must record it itself.
- Callbacks may be invoked while the caller's kernel lock is held. They must
  not re-enter the kernel.
- Objects are freed in reverse order of allocation within a module, but the
  library guarantees exact-size bookkeeping rather than order-dependent
  behavior.
- The allocator context must outlive the kernel.

The library's internal memory model is: one mutable kernel, per-document
history records, per-snapshot pinned blocks, per-subscription rings. Every
allocation can fail independently, and the library holds the document state
consistent whenever that happens (see 2.2's `CDIN_NO_MEMORY` row).

### 5.2 `cdin_lock_vtable` (optional)

```c
typedef struct cdin_lock_vtable {
    uint32_t struct_size;
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} cdin_lock_vtable;
```

- `struct_size` must be 0 (unused) or exactly `sizeof(cdin_lock_vtable)`.
- `lock` and `unlock` must be both NULL or both non-NULL; otherwise the
  argument pair is rejected.
- `ctx` and any callback must be all-zero when `struct_size` is 0.
- A lock is taken at the entry of a public call and released at its exit,
  except for the scan phase of a running search (which releases the lock, pins
  the document, and re-locks before reporting results) and callbacks listed
  below.
- The kernel never recurses into a lock and never calls user callbacks while
  holding it, with exactly these exceptions: the allocator may run under the
  lock, the log sink never does, and the wakeup callback never does.

### 5.3 `cdin_log_sink` (optional)

```c
typedef struct cdin_log_sink {
    uint32_t struct_size;
    void *ctx;
    void (*log)(void *ctx, int level, const char *message, size_t len);
} cdin_log_sink;
```

- `struct_size` 0 or exact; zeroing the fields is mandatory when 0.
- `level`: 0 error, 1 warning, 2 info.
- `message` is UTF-8, not NUL-terminated; read exactly `len` bytes.
- In 1.0.0 the only message is a warning emitted when a kernel is destroyed
  while snapshots are still live.

## 6. Struct reference

Contract-oriented listing of every public struct, in the order the header
declares them. Fields not listed under contract notes have no special
constraints beyond `struct_size`.

### 6.1 `cdin_kernel_config`

Declared with these limits as flat `uint64_t` fields (in order):
`max_documents`, `max_document_size`, `max_edit_size`, `max_undo_memory`,
`max_blob_size`, `max_snapshots_per_doc`, `max_subscribers_per_doc`,
`notif_queue_capacity`, `default_search_steps`.

Zeroed limit fields select defaults (see user guide section 16). Two hard
caps are applied by the library: subscribers clamp at 64 and notification
queue capacity clamps to `[4, 65536]`. The document-size default is clamped
to `SIZE_MAX/4`. `abi_major` selects the ABI contract; it must equal
`CDIN_KERNEL_ABI_MAJOR`.

### 6.2 `cdin_replace_request`

| Field | Constraint |
|---|---|
| `flags` | must be 0 |
| `expected_version` | current version, or `CDIN_ANY_VERSION` |
| `start`, `end` | `start <= end <= length`; `end` may equal `length` |
| `bytes` | may be NULL only when `len == 0` |
| `len` | `0 <= len <= max_edit_size` |
| `coalesce_key` | any non-zero value enables coalescing with a matching key |
| `blob_before`/`blob_after` | optional, each `<= max_blob_size` |

### 6.3 `cdin_reload_request`

| Field | Constraint |
|---|---|
| `flags` | must be 0; `CDIN_RELOAD_KEEP_HISTORY` is defined but unsupported |
| `expected_version` | current version, or `CDIN_ANY_VERSION` |
| `bytes`, `len` | NULL only when `len == 0`; `len <= max_document_size` |

### 6.4 `cdin_batch_request`

| Field | Constraint |
|---|---|
| `flags` | must be 0 |
| `expected_version` | current version, or `CDIN_ANY_VERSION` |
| `count` | `0 <= count <= CDIN_MAX_BATCH_EDITS_HARD (65536)` |
| `starts`, `ends` | each `start <= end`; sorted ascending; pairwise non-overlapping; adjacent allowed |
| `bytes` | array of `count` pointers; `bytes[i]` may be NULL only when `lens[i] == 0` |
| `lens` | array of `count` sizes; each `<= max_edit_size`; sum `<= max_edit_size` |
| `coalesce_key` | must be 0 |
| `blob_before`/`blob_after` | optional, per unit |

### 6.5 `cdin_edit_result`

Output. Caller sets `struct_size`; all other fields written only after a
mutating call returns. `details` is zeroed at the start of each call. Fields
`start`, `removed`, `inserted` and `blob_len` are 0 when a no-op fails or when
`edit_count != 1`.

### 6.6 `cdin_history_request` (undo/redo)

| Field | Constraint |
|---|---|
| `flags` | must be 0 |
| `expected_version` | current version, or `CDIN_ANY_VERSION` |
| `blob_out` | optional; may be NULL |
| `blob_capacity` | used only if `blob_out != NULL` |

`blob_len` in the result carries the stored blob size (regardless of copy).
`CDIN_BUFFER_TOO_SMALL` sets `details.required` to the needed bytes; the check
occurs before the version check.

### 6.7 `cdin_txn_begin_request` and `cdin_txn_commit_request`

Begin: `flags` 0, `expected_version` current (or `CDIN_ANY_VERSION`).
Commit: `flags` 0, optional blob pair, each `<= max_blob_size`. A transaction
commit with no pending edits returns `CDIN_OK`, `edit_count == 0`, and does
not change the version or consume a sequence number.

### 6.8 `cdin_eol_info`

Caller sets `struct_size`; the struct is overwritten on success. Empty
documents report zero terminators, `final_line_unterminated == 0`, and
`mixed == 0`.

### 6.9 `cdin_subscribe_request`

| Field | Constraint |
|---|---|
| `flags` | must be 0 |
| `wakeup`, `wakeup_ctx` | optional; wakeup is invoked once per mutation, after queueing, with the caller lock released |

### 6.10 `cdin_notif_event` and constants

`cdin_notif_kind` numeric values are ABI-stable:

| Value | Kind |
|---|---|
| 0 | `CDIN_NOTIF_EDIT` |
| 1 | `CDIN_NOTIF_BATCH` |
| 2 | `CDIN_NOTIF_TRANSACTION` |
| 3 | `CDIN_NOTIF_UNDO` |
| 4 | `CDIN_NOTIF_REDO` |
| 5 | `CDIN_NOTIF_RELOAD` |
| 6 | `CDIN_NOTIF_CLOSED` |
| 7 | `CDIN_NOTIF_HISTORY_EVICTED` |
| 8 | `CDIN_NOTIF_RESYNC` |
| 9 | `CDIN_NOTIF_SAVED_STATE_CHANGED` |

`CDIN_NOTIF_MAX_EDITS == 8`; `edits_stored == min(edit_count, 8)`;
`edits_truncated != 0` invalidates the description list.

### 6.11 `cdin_search_request` / `cdin_search_result`

See user guide section 12 for the complete field meanings. The mode and
direction constants (`CDIN_SEARCH_{FORWARD, BACKWARD}`,
`CDIN_SEARCH_{LITERAL, BYTES, REGEX}`) and the `CDIN_SEARCH_*` flag constants
are stable numeric values; regex is declared but not implemented in 1.0.0.

Matches are written to `req.ranges` (the request's array), never to the
result struct; the result carries only `count` and the continuation fields.

## 7. Function reference

Every entry returns `cdin_status`; NULL kernel argument is always
`CDIN_INVALID_ARGUMENT` unless documented otherwise.

### Kernel

| Function | Contract summary |
|---|---|
| `cdin_abi_version(&maj,&min,&pat)` | Compile-time constants of the loaded library. |
| `cdin_features(&mask)` | Feature bit-set of the loaded library. |
| `cdin_unicode_version(&maj,&min,&pat)` | Unicode data version (15.1.0 in 1.0.0). |
| `cdin_status_name(s)` | Static name of a status; never NULL. |
| `cdin_kernel_create(cfg, &k)` | Validates config, builds kernel; see 6.1 and 5. |
| `cdin_kernel_destroy(k)` | Frees everything; must not overlap other calls. |
| `cdin_kernel_shutdown(mode)` | Stops mutations, aborts active transactions. |
| `cdin_kernel_last_sequence(&seq)` | Monotonic counter of accepted requests. |

### Documents

| Function | Contract summary |
|---|---|
| `cdin_doc_create(k, bytes, len, &d)` | Copies bytes; empty allowed; version 0. |
| `cdin_doc_close(k, d)` | Emits CLOSED; aborts active txn; invalidates handle. |
| `cdin_doc_reload(k, d, req, out)` | Full replace; clears history; marks clean. |
| `cdin_doc_replace(k, d, req, out)` | One range replacement. |
| `cdin_doc_replace_batch(k, d, req, out)` | Atomic batch; one history unit. |
| `cdin_doc_undo(k, d, req, out)` / `cdin_doc_redo` | History movement; blob copy-out. |
| `cdin_doc_mark_clean(k, d)` | Saves state; seq consumed; no event when already clean. |
| `cdin_doc_is_clean(k, d, int*)` | Query saved-state. |
| `cdin_doc_set_metadata(k, d, v)` / `get_metadata` | Opaque caller 64-bit value; no version effect. |
| `cdin_doc_history_info(k, d, &undo,&redo,&evict,&bytes)` | Bounded counters. |

### Transactions

| Function | Contract summary |
|---|---|
| `cdin_txn_begin(k, d, req, &t)` | One per document; seq consumed; version check. |
| `cdin_txn_replace(k, t, req, out)` | Tentative edit; no sequence; checks against tentative content. |
| `cdin_txn_commit(k, t, req, out)` | Publishes as one edit; consumes seq; one event. |
| `cdin_txn_abort(k, t)` | Discards tentative content; no event. |
| `cdin_txn_len(k, t, &n)` / `cdin_txn_read(...)` | Tentative-length query/read. |

### Snapshots

| Function | Contract summary |
|---|---|
| `cdin_snapshot_acquire(k, d, &ver, &s)` | Immutable pin; counts against per-doc limit. |
| `cdin_snapshot_release(k, s)` | Explicit release; handle invalid after. |

### Reading

All read functions take `cdin_source`; all honor the `start <= end <= length`
range rule and the `buf == NULL` size-query pattern where the signature
includes `buf`.

| Function | Contract summary |
|---|---|
| `cdin_text_version(k, src, &v)` | Version of the source. |
| `cdin_text_len(k, src, &n)` | Byte length; O(1). |
| `cdin_text_read(..., flags)` | Range read; flags report mid-sequence range boundaries. |
| `cdin_text_serialize(...)` | Whole-document read. |
| `cdin_text_check_position(k, src, ver, off)` | Version-checked position validation. |
| `cdin_text_region_at(k, src, off, &s,&e,&raw)` | Maximal same-validity region around a byte. |
| `cdin_text_line_count/line_start/line_from_offset` | Line index helpers. |
| `cdin_text_eol_info(k, src, out)` | EOL style statistics. |
| `cdin_text_byte_to_codepoint/codepoint_to_byte` | Conversions; `CDIN_CONV_TOUCHED_RAW` on raw crossing. |
| `cdin_text_codepoint_next/prev` | Unit boundary snapping. |
| `cdin_text_grapheme_next/prev` | Extended-cluster boundary snapping. |

### Search

| Function | Contract summary |
|---|---|
| `cdin_search(k, req, out)` | Literal/byte search with pagination, steps, cancellation. |

### Notifications

| Function | Contract summary |
|---|---|
| `cdin_notif_subscribe(k, d, req, &v, &n)` | Bounded per-subscriber queue; optional wakeup. |
| `cdin_notif_unsubscribe(k, n)` | Frees queue; invalidates handle. |
| `cdin_notif_drain(k, n, evs, max, &count)` | FIFO copy; `max == 0` is allowed (no copy, count 0). |

## 8. Threading invariants

The library is thread-agnostic: it spawns nothing and owns no synchronization
primitives. The caller decides the concurrency model:

1. **No lock configured.** Every call must run on the same thread, or the
   caller must serialize calls externally before they enter the library.
2. **Lock configured.** The pair is acquired/released around each call. The
   lock must be a mutual-exclusion lock; it need not be recursive. The
   allocator callbacks may then be invoked under the lock; log and wakeup
   callbacks never are.
3. **Search.** A search acquires the lock to resolve and pin the source,
   releases it for the scan, and re-acquires it to write results. This keeps
   scanning off the lock while preserving correctness.
4. **Destruction.** `cdin_kernel_destroy` requires external exclusivity: no
   other call on the kernel may be in flight, even with locks (the library
   does not lock during destroy because there is nothing to protect after
   teardown).

HANDLE RULES ACROSS THREADS: handles are plain 64-bit values; you may pass
them between threads, but the *use* of a handle follows the concurrency rule
of the configured lock model. `cdin_notif` wakeup callbacks are exempt: they
may run on any thread that performed a mutation.

## 9. ABI stability policy

- **ABI major 1.** Numeric values, struct layouts, and function signatures
  that exist today are frozen. Nothing standing may change: no field
  reordering, no semantic change of a documented status returned for an
  input, no signature change.
- **Minor growth.** Adding functions, adding feature bits, and adding
  *trailing* fields to a struct with `struct_size` are minor bumps. The
  consequences of the `struct_size` protocol (section 4) are:
  - a caller rebuilt against the new minor version works on both the old and
    the new library, as long as it zero-fills the tail it does not need;
  - a caller built for an older minor version fails explicitly
    (`CDIN_INVALID_ARGUMENT`) on a newer library; recompile against the new
    header. There is no silent struct reinterpretation.
- **Embedded sub-tables** (`cdin_allocator`, `cdin_lock_vtable`,
  `cdin_log_sink`) cannot grow this way in ABI 1; they require an exact
  `struct_size`. If they ever change, there will be a major bump or a new
  top-level field.
- **Enums never renumber.** New values are appended; feature bits likewise.
- **`CDIN_KERNEL_BUILD_DLL`** builds the shared library
  (`-fvisibility=hidden` outside Windows, `__declspec(dllexport)` on
  Windows); consumer builds must never define it. The static library exports
  `CDIN_API` symbols with default visibility.
- The header compiles as C11 and as C++ through `extern "C"`. It requires
  only `<stdint.h>` and `<stddef.h>`.

To verify the library you linked against matches the header you compiled
with, check at startup:

```c
uint32_t major = 0, minor = 0, patch = 0;
if (cdin_abi_version(&major, &minor, &patch) != CDIN_OK ||
    major != CDIN_KERNEL_ABI_MAJOR) {
    /* refuse to run: rebuild or relink against the matching version */
}
```

Minor mismatch is tolerated as long as calls you use exist in the loaded
minor version (that's the struct_size and feature-mask protocol).

