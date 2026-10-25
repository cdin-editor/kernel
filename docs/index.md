# cdin-kernel documentation

`cdin-kernel` is a portable C11 library for binary-safe text editing. It owns
documents as byte sequences, applies edits atomically, keeps bounded undo
history, publishes immutable snapshots, converts text positions, searches
literals and arbitrary bytes, and queues per-document change notifications.
It does not own files, windows, or threads; the application supplies memory,
optionally supplies a lock, and decides when and where calls run.

These documents describe C ABI **1.0.0** and Unicode data **15.1.0**. The
authoritative interface is [`include/cdin/kernel.h`](../include/cdin/kernel.h).
Where this documentation and the header disagree, the header is correct; please
report the mismatch.

## Which document to read

| If you want to | Read |
|---|---|
| Use the library from C, understand every operation | [User guide](user-guide.md) |
| Call the library from Python, Go, JavaScript, TypeScript, C++, or another runtime | [Language bindings](language-bindings.md) |
| Build, install, link, or package the library | [Installation and build](installation.md) |
| Look up an exact type, struct, status value, or function contract | [C ABI reference](abi.md) |
| Change the library, add a function, add a test | [Developer guide](development.md) |
| Understand the internal data structures and invariants | [Architecture](architecture.md) |

The suggested order for a new user is: this page, the first three sections of
the user guide, then the specific operations you need. For a new contributor:
the user guide for behavior, the architecture for internals, then the developer
guide for the build and test loop.

## Vocabulary used throughout

- **Byte offset** - an index into the stored bytes. All offsets and ranges in
  the public API are byte-based, not character-based.
- **Range** - half-open, `[start, end)`. `start == end` is an empty range at a
  position; `start < end` covers `end - start` bytes.
- **Version** - a per-document counter. A new document starts at version `0`;
  each accepted content change increments it by exactly one.
- **Handle** - an opaque 64-bit value identifying a document, transaction,
  snapshot, or subscription. Zero is never a valid handle.
- **Accepted** - a request that passed validation and the version check and
  received a sequence number. An accepted request can still fail later while
  allocating memory; in that case the document does not change.
- **Sequence number** - kernel-wide ordering metadata for accepted requests.
  Gaps are possible after a later failure. It is not a cross-thread timestamp.

## Rules that hold everywhere

1. Every public function returns `cdin_status`; `CDIN_OK` is zero. Results are
   returned through output parameters.
2. Every struct that declares `struct_size` must have it set by the caller
   before the call. See the ABI reference for the exact size rules.
3. The library copies input bytes before returning. It never returns an
   internal pointer and never takes ownership of a caller buffer.
4. Failed calls leave output values untouched, except for the documented
   `details` fields and buffer-size outputs.
5. Text is bytes. The library never rewrites, normalizes, or sanitizes content
   on load, store, or copy.
6. A zero-valued limit in the kernel configuration selects the documented
   default.
7. The same kernel must not be used concurrently unless a lock vtable is
   configured; `cdin_kernel_destroy` must never overlap another call.

## Documentation conventions

- "Must" marks a requirement; violating it is a bug in the caller. "Should"
  marks a recommendation.
- Tables list defaults and hard caps from the header; they are not estimates.
- Code examples target C11. They compile against the static library produced by
  `make`; see [Installation](installation.md).
- All statements about behavior in this directory were checked against the
  source in this repository. Checks that were not run are marked `NOT RUN` in
  the developer guide; no result is claimed from reading code alone.
