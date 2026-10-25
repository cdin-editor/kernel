# User guide

This guide covers the public behavior of `cdin-kernel` C ABI 1.0.0. It is written
for an application programmer who stores text, applies edits, undoes them,
searches, and reacts to changes. Every function name, default, and error
condition below was checked against `include/cdin/kernel.h` and the
implementation in this repository.

Read [Core ideas](#2-core-ideas) and [Editing safely](#7-editing-safely) first
if you read only two sections. Use the table of contents to jump to an
operation.

## Contents

1. [What the library is](#1-what-the-library-is)
2. [Core ideas](#2-core-ideas)
3. [Your first program](#3-your-first-program)
4. [Kernel setup and teardown](#4-kernel-setup-and-teardown)
5. [Documents](#5-documents)
6. [Reading bytes](#6-reading-bytes)
7. [Editing safely](#7-editing-safely)
8. [Undo, redo, and saved state](#8-undo-redo-and-saved-state)
9. [Transactions](#9-transactions)
10. [Snapshots](#10-snapshots)
11. [The text model](#11-the-text-model)
12. [Search](#12-search)
13. [Notifications](#13-notifications)
14. [Threads](#14-threads)
15. [Status codes and failures](#15-status-codes-and-failures)
16. [Limits](#16-limits)
17. [Recipes](#17-recipes)
18. [Edge cases and answers](#18-edge-cases-and-answers)

---

## 1. What the library is

`cdin-kernel` is a text engine, not an editor application.

It provides:

- documents that store arbitrary bytes and track a version;
- atomic edits of byte ranges, including one-call batch edits;
- bounded undo and redo history, with caller-defined opaque blobs per unit;
- transactions that publish several edits as one mutation;
- immutable snapshots for stable reads;
- line, end-of-line, code-point, and grapheme-cluster helpers;
- literal UTF-8 search, byte-exact search, result pagination, step limits, and
  cancellation;
- bounded per-document change notifications with resynchronization on overflow.

It does not provide:

- file loading or saving (the application reads and writes files);
- windowing, rendering, input handling, or syntax highlighting;
- threads, timers, or a main loop (the application runs all of these);
- regular-expression search in this release (`CDIN_SEARCH_REGEX` returns
  `CDIN_UNSUPPORTED`);
- Unicode normalization, collation, or locale-aware case mapping;
- a cursor or selection model (the application owns cursors and asks the
  library for boundaries and positions).

The library is C11, has no required external dependencies, allocates only
through the allocator the application supplies, and never spawns a thread or a
process.

## 2. Core ideas

### 2.1 Kernel

A kernel is the owner of all library state: the document table, undo memory,
snapshots, transactions, subscriptions, and the sequence counter. Create one
with `cdin_kernel_create` and destroy it with `cdin_kernel_destroy`. All other
calls take the kernel as their first argument.

One kernel can own many documents. Different kernels share nothing. A kernel
is cheap to create, so a program that edits unrelated corpora in parallel can
use one kernel per worker instead of sharing a kernel with a lock.

### 2.2 Documents, versions, and snapshots

A document is a byte sequence plus state:

- version (`cdin_version`): starts at `0`; every accepted content change adds
  exactly one;
- undo/redo history, clean marker, metadata, and a count of open snapshots;
- subscribers waiting for change events.

A snapshot is an immutable view of one document version. It keeps the old
content alive while the document moves on. Reads and searches work on both
documents and snapshots through `cdin_source`.

### 2.3 Handles

Documents, transactions, snapshots, and subscriptions are addressed by opaque
64-bit handles (`cdin_doc`, `cdin_txn`, `cdin_snapshot`, `cdin_notif`). Zero is
never valid. A handle becomes invalid when the object is closed, committed,
aborted, released, or unsubscribed, and when the kernel is destroyed.

Handles contain a slot index and a generation counter. Reusing a slot does not
revive an old handle: the generation differs, so the stale handle returns
`CDIN_INVALID_HANDLE`. Handles do not carry a type tag; a document handle and a
snapshot handle can have the same numeric value. Always pass a handle to the
functions for the object that produced it.

### 2.4 Byte offsets and ranges

All positions, lengths, and ranges are byte-based. A range is half-open:
`[start, end)` covers bytes `start` through `end - 1`. `start == end` is an
empty range at `start`; it is a valid position for an insertion. A range with
`start > end` is invalid.

Every byte offset in `[0, length]` is a valid position for reading and for
insertion. A position strictly inside a well-formed multi-byte UTF-8 sequence
is not a valid *edit* position: replacing there would split a character and is
rejected with `CDIN_INVALID_POSITION`. Bytes that are not part of a well-formed
sequence are called *raw*; every raw byte is a valid position. Section
[11](#11-the-text-model) defines the model exactly.

### 2.5 Versions and optimistic concurrency

Every mutating operation that takes a version carries `expected_version`:

- `CDIN_ANY_VERSION` (`UINT64_MAX`) disables the check;
- any other value must equal the document's current version, or the call fails
  with `CDIN_STALE_VERSION` and does not change the document.

The check and the mutation are serialized by the kernel, so two writers that
both expect version 7 cannot both succeed: the first produces version 8, the
second gets `CDIN_STALE_VERSION` with `details.expected = 7` and
`details.current = 8`. A caller that does not care about lost updates can pass
`CDIN_ANY_VERSION`; a caller that merges concurrent edits should re-read and
retry on `CDIN_STALE_VERSION`.

Snapshots make a read-modify-write loop safe without holding a lock: acquire a
snapshot, read and compute from it, then submit the edit with the snapshot's
version as `expected_version`.

### 2.6 Status codes and output parameters

Every function returns `cdin_status`; `CDIN_OK` is zero. Results are written to
caller-provided output parameters. The usual pattern is:

```c
cdin_edit_result result;
memset(&result, 0, sizeof result);
result.struct_size = (uint32_t)sizeof result;

cdin_status st = cdin_doc_replace(kernel, doc, &request, &result);
if (st != CDIN_OK) {
    /* result is not a valid edit; read only result.details if documented */
}
```

Output parameters are written only on success, except for the documented
`details` fields and the size outputs used with `CDIN_BUFFER_TOO_SMALL`. You
may pass `NULL` for an optional output parameter where the header says so.

### 2.7 `struct_size`

Every public struct that has a `struct_size` field must have it set by the
caller before the call:

```c
memset(&request, 0, sizeof request);
request.struct_size = (uint32_t)sizeof request;
```

For input structs, `struct_size` must be at least the size known to the
library. A larger value (a caller built against a newer header running on an
older library) is accepted only if the extra bytes are zero. A smaller value,
a caller built against an older header, is rejected with
`CDIN_INVALID_ARGUMENT`, so a mismatched program fails explicitly, never by
misreading a struct. For `cdin_edit_result`, `cdin_search_result`, and
`cdin_eol_info`, the caller sets `struct_size` before the call and the library
writes the rest.

The nested `cdin_allocator`, `cdin_lock_vtable`, and `cdin_log_vtable` are
special: in ABI 1.0.0 each must have `struct_size` equal to its exact size, or
zero for the optional lock and log tables. The ABI reference lists the rule for
every struct.

### 2.8 Sequence numbers

`cdin_kernel_last_sequence` reports the kernel-wide counter of accepted
requests. Sequence numbers are ordering metadata, not arrival timestamps and
not document versions. They are assigned to accepted content mutations,
undo/redo, reload, saved-state marking, and transaction begin/commit. A request
that passes validation and the version check but fails later while allocating
memory still consumes its sequence number, so gaps are possible by design.
`accepted_seq` in `cdin_edit_result` is nonzero when a request was accepted,
even if a later step failed.

## 3. Your first program

The following program creates a kernel, creates a document containing
`hello`, replaces it with `Hello, CDIN`, reads the bytes back, and closes
everything. It is a complete C11 program.

```c
#include <cdin/kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel receives memory through these three callbacks. */
static void *app_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void *app_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size; /* realloc() does not need the old size */
    return realloc(ptr, new_size);
}

static void app_free(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    (void)size; /* free() does not need the size */
    free(ptr);
}

#define CHECK(expr) do {                                                   \
    cdin_status st_ = (expr);                                              \
    if (st_ != CDIN_OK) {                                                  \
        fprintf(stderr, "%s failed: %s\n", #expr, cdin_status_name(st_));  \
        return 1;                                                          \
    }                                                                      \
} while (0)

int main(void)
{
    cdin_kernel_config cfg;
    cdin_kernel *kernel = NULL;
    cdin_doc doc = 0;
    cdin_replace_request request;
    cdin_edit_result result;
    uint64_t length = 0;
    uint8_t *buffer = NULL;
    size_t read = 0;
    cdin_status st;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator.struct_size = (uint32_t)sizeof cfg.allocator;
    cfg.allocator.alloc = app_alloc;
    cfg.allocator.realloc = app_realloc;
    cfg.allocator.free = app_free;
    CHECK(cdin_kernel_create(&cfg, &kernel));

    CHECK(cdin_doc_create(kernel, (const uint8_t *)"hello", 5, &doc));

    memset(&request, 0, sizeof request);
    request.struct_size = (uint32_t)sizeof request;
    request.start = 0;
    request.end = 5;
    request.bytes = (const uint8_t *)"Hello, CDIN";
    request.len = 11;
    request.expected_version = 0;

    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;
    CHECK(cdin_doc_replace(kernel, doc, &request, &result));
    printf("version=%llu length=%llu\n",
           (unsigned long long)result.new_version,
           (unsigned long long)result.new_length);

    CHECK(cdin_text_len(kernel, cdin_src_doc(doc), &length));
    buffer = malloc((size_t)length);
    if (!buffer) return 1;
    CHECK(cdin_text_read(kernel, cdin_src_doc(doc), 0, length,
                         buffer, (size_t)length, &read, NULL));
    fwrite(buffer, 1, (size_t)read, stdout);
    fputc('\n', stdout);
    free(buffer);

    CHECK(cdin_doc_close(kernel, doc));
    st = cdin_kernel_destroy(kernel);
    return st == CDIN_OK ? 0 : 1;
}
```

Compile it against the repository build:

```sh
make
cc -std=c11 -Iinclude app.c build/libcdin_kernel.a -o app
./app
```

The output is:

```text
version=1 length=11
Hello, CDIN
```

The example shows the complete lifecycle. The sections below explain every
choice in it and the operations it does not use.

## 4. Kernel setup and teardown

### 4.1 Configuration

A kernel is created from `cdin_kernel_config`. Zero the struct, set
`struct_size`, set `abi_major` to `CDIN_KERNEL_ABI_MAJOR`, and fill in the
allocator. Every other field has a default when left at zero.

| Field | Default when zero | Meaning |
|---|---|---|
| `allocator` | required | Memory callbacks; see 4.2. |
| `lock` | no locking | Optional lock/unlock pair; see 4.3. |
| `log` | no logging | Optional diagnostic callback. |
| `max_documents` | 1024 | Maximum live documents. |
| `max_document_size` | 1 GiB | Maximum bytes in one document. |
| `max_edit_size` | `max_document_size` | Maximum inserted bytes in one edit; also caps the total inserted bytes of a batch. |
| `max_undo_memory` | 64 MiB | Undo/redo memory per document; also the tentative-edit cap of a transaction. |
| `max_blob_size` | 4096 | Maximum size of one caller blob (`before` or `after`). |
| `max_snapshots_per_doc` | 16 | Live snapshots per document. |
| `max_subscribers_per_doc` | 16 | Subscriptions per document; values above 64 are clamped to 64. |
| `notif_queue_capacity` | 64 | Events queued per subscriber; clamped to 4..65536. |
| `default_search_steps` | 2^32 | Step budget when a search request has `step_limit == 0`. |

The configuration is copied; the kernel does not keep a pointer to it. The
lock and log contexts are not copied; they must outlive the kernel.

### 4.2 Allocator contract

The three callbacks replace every allocation the library makes:

```c
typedef struct cdin_allocator {
    uint32_t struct_size;
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} cdin_allocator;
```

Rules:

- `struct_size` must equal `sizeof(cdin_allocator)` in ABI 1.0.0.
- All three function pointers must be non-NULL.
- `size` is never zero for `alloc`.
- `free` receives the exact size that was allocated or last reallocated.
- `realloc` receives the exact old and new sizes. It must follow standard
  `realloc` semantics: on failure it returns `NULL`, leaves the old block
  valid, and does not free it. The kernel then frees the old block and reports
  `CDIN_NO_MEMORY`.
- Callbacks may be called while the kernel lock is held. They must not call
  back into the kernel; a non-recursive lock would deadlock.
- Callbacks may be called from any thread that calls the kernel, so a shared
  allocator must be thread-safe. A per-kernel allocator used with a lock
  vtable is also called under that lock.
- The library does not use `malloc`, `calloc`, `realloc`, or `free` directly.

Keep the allocator and its `ctx` alive until `cdin_kernel_destroy` returns.

### 4.3 Lock and log

`cdin_lock_vtable` is optional. Set both callbacks or neither:

```c
typedef struct cdin_lock_vtable {
    uint32_t struct_size;
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} cdin_lock_vtable;
```

When configured, the kernel acquires the lock at the start of a public call and
releases it before returning, except for `cdin_kernel_destroy` (which must be
exclusive by itself) and the portion of a search that scans a pinned snapshot.
Lock callbacks must implement mutual exclusion, must not call the kernel, and
should be short. The kernel does not create threads and does not require a
recursive lock. See [Threads](#14-threads).

`cdin_log_sink` is also optional:

```c
typedef struct cdin_log_sink {
    uint32_t struct_size;
    void *ctx;
    void (*log)(void *ctx, int level, const char *message, size_t len);
} cdin_log_sink;
```

`level` is 0 for errors, 1 for warnings, and 2 for information. `message` is
UTF-8 and is not NUL-terminated; use `len`. The pointer is valid only for the
duration of the call. Log callbacks run without the kernel lock. In ABI 1.0.0
the kernel emits one warning: destroying a kernel that still has live
snapshots.

### 4.4 Creating a kernel

```c
cdin_kernel_config cfg;
memset(&cfg, 0, sizeof cfg);
cfg.struct_size = (uint32_t)sizeof cfg;
cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
cfg.allocator.struct_size = (uint32_t)sizeof cfg.allocator;
cfg.allocator.alloc = app_alloc;
cfg.allocator.realloc = app_realloc;
cfg.allocator.free = app_free;

cdin_kernel *kernel = NULL;
cdin_status st = cdin_kernel_create(&cfg, &kernel);
```

`cdin_kernel_create` validates, in order: `out` non-NULL, `cfg` and its
declared size, `abi_major`, the allocator, the lock pair and its size, the log
sink and its size, then the limits. It returns:

| Status | Cause |
|---|---|
| `CDIN_INVALID_ARGUMENT` | NULL pointer, bad `struct_size`, missing allocator callback, half a lock pair, or a non-zero context with `struct_size == 0`. |
| `CDIN_VERSION_MISMATCH` | `abi_major` is not `CDIN_KERNEL_ABI_MAJOR`. |
| `CDIN_UNSUPPORTED` | `struct_size` is larger than the known struct and the extra bytes are non-zero. |
| `CDIN_NO_MEMORY` | The allocator returned `NULL` for the kernel or its first tables. |

On failure `*out` is untouched.

### 4.5 Destroying a kernel

`cdin_kernel_destroy` frees every document, transaction, snapshot, subscriber,
history unit, and cached index owned by the kernel, then frees the kernel
itself. It emits no notifications and performs no I/O.

- It must not run concurrently with any other call on the same kernel, even
  when a lock vtable is configured.
- Handles and snapshots become invalid immediately; using one afterwards is
  undefined behavior because the kernel memory is gone.
- Closing documents first is optional. If snapshots are still live, the kernel
  logs a warning and releases them.
- Destroying `NULL` returns `CDIN_INVALID_ARGUMENT`.

### 4.6 Shutdown

`cdin_kernel_shutdown` stops the kernel without freeing it. Both mode values
(`CDIN_SHUTDOWN_GRACEFUL` and `CDIN_SHUTDOWN_ABORT`) behave identically in ABI
1.0.0 because every call is synchronous and there is no queued work.

Shutdown:

- marks the kernel stopped, so new content mutations and object creation fail
  with `CDIN_SHUTDOWN`;
- aborts and rolls back every active transaction (their handles become
  invalid);
- does not close documents, release snapshots, or unsubscribe subscribers.

The following calls keep working after shutdown:

| Operation | After shutdown |
|---|---|
| `cdin_doc_close` | Works; emits `CLOSED` to subscribers. |
| All `cdin_text_*` reads, on documents and snapshots | Work. |
| `cdin_search` | Works. |
| `cdin_snapshot_release` | Works. |
| `cdin_notif_drain`, `cdin_notif_unsubscribe` | Work. |
| `cdin_doc_is_clean`, `cdin_doc_get_metadata`, `cdin_doc_history_info` | Work. |
| `cdin_kernel_last_sequence`, version and feature queries | Work. |
| `cdin_kernel_destroy` | Works. |

These fail with `CDIN_SHUTDOWN`:

| Operation | After shutdown |
|---|---|
| `cdin_doc_create` | `CDIN_SHUTDOWN` |
| `cdin_doc_replace`, `cdin_doc_replace_batch` | `CDIN_SHUTDOWN` |
| `cdin_doc_undo`, `cdin_doc_redo` | `CDIN_SHUTDOWN` |
| `cdin_doc_reload`, `cdin_doc_mark_clean`, `cdin_doc_set_metadata` | `CDIN_SHUTDOWN` |
| `cdin_snapshot_acquire` | `CDIN_SHUTDOWN` |
| `cdin_notif_subscribe` | `CDIN_SHUTDOWN` |
| `cdin_txn_begin` and all `cdin_txn_*` calls | `CDIN_SHUTDOWN` |

Shutdown is idempotent. It does not return until all currently running calls
have completed (they hold the lock; shutdown waits for it).

## 5. Documents

### 5.1 Creating a document

```c
cdin_doc doc = 0;
cdin_status st = cdin_doc_create(kernel, bytes, len, &doc);
```

`bytes` is copied; the caller may free it immediately after the call. `len == 0`
creates an empty document and `bytes` may be `NULL`; otherwise `bytes` must be
non-NULL. The new document has version `0`, no history, and a clean saved
state. On success `*doc` receives a non-zero handle. On failure `*doc` is
untouched.

| Status | Cause |
|---|---|
| `CDIN_SHUTDOWN` | The kernel has been shut down. |
| `CDIN_INVALID_ARGUMENT` | NULL kernel or output, or `bytes == NULL` with `len > 0`. |
| `CDIN_LIMIT_EXCEEDED` | `max_documents` reached, or `len > max_document_size`. |
| `CDIN_NO_MEMORY` | Allocation failed. |

### 5.2 Closing a document

`cdin_doc_close(kernel, doc)` closes one document and invalidates its handle.
It aborts an active transaction, discards undo/redo history, emits `CLOSED` to
each subscriber, and detaches the subscriptions. Snapshots of the document stay
valid because they hold their own reference to the content. Closing an already
closed document returns `CDIN_INVALID_HANDLE`. Closing a document after kernel
shutdown is allowed.

### 5.3 Replacing the whole content

`cdin_doc_reload` replaces the content in one operation:

```c
cdin_reload_request req;
memset(&req, 0, sizeof req);
req.struct_size = (uint32_t)sizeof req;
req.expected_version = version;
req.bytes = data;
req.len = len;

cdin_edit_result result;
memset(&result, 0, sizeof result);
result.struct_size = (uint32_t)sizeof result;
st = cdin_doc_reload(kernel, doc, &req, &result);
```

Properties:

- the call fails with `CDIN_BUSY` if a transaction is active;
- `flags` must be `0`; `CDIN_RELOAD_KEEP_HISTORY` is defined but not supported
  and returns `CDIN_UNSUPPORTED`;
- all undo and redo units are discarded;
- the document becomes clean, even if it was dirty before;
- the version increments by one;
- subscribers receive `RELOAD`, then `SAVED_STATE_CHANGED` if the document was
  dirty before the reload;
- `result.edit_count` is `0`; `result.new_length` is the new length;
- `result.start`, `removed`, and `inserted` are `0`.

`len > max_document_size` returns `CDIN_LIMIT_EXCEEDED` with
`details.limit_id == CDIN_LIMIT_DOCUMENT_SIZE`.

### 5.4 Metadata

Each document carries one caller-defined 64-bit value:

```c
cdin_doc_set_metadata(kernel, doc, 0x1234);
uint64_t value = 0;
cdin_doc_get_metadata(kernel, doc, &value);
```

Metadata is not content: it does not change the version, does not enter
history, and produces no notification. Setting metadata is a mutation and fails
with `CDIN_SHUTDOWN` after shutdown; reading it still works. The initial value
is `0`.

### 5.5 History information

```c
uint64_t undo_units = 0, redo_units = 0, evicted = 0, bytes = 0;
st = cdin_doc_history_info(kernel, doc, &undo_units, &redo_units,
                           &evicted, &bytes);
```

Every output is optional; pass `NULL` for the values you do not need.

| Output | Meaning |
|---|---|
| `undo_units` | Units that can be undone. |
| `redo_units` | Units that can be redone. |
| `evicted` | Units discarded by the memory cap since the document was created. This does not count redo units dropped by a new edit. |
| `bytes` | Memory currently accounted to history: edit records, removed and inserted bytes, and caller blobs. |

The accounting is exact for what the library stores; it does not include
allocator overhead or the per-document table entry.

## 6. Reading bytes

### 6.1 Sources

Every read function takes `cdin_source`, which names either a document or a
snapshot:

```c
cdin_source from_doc = cdin_src_doc(doc);
cdin_source from_snapshot = cdin_src_snap(snapshot);
```

Exactly one field must be non-zero. Both zero, or both non-zero, returns
`CDIN_INVALID_ARGUMENT`. Use the two helper functions rather than initializing
the struct by hand.

### 6.2 Version and length

```c
cdin_version version = 0;
uint64_t length = 0;
cdin_text_version(kernel, source, &version);
cdin_text_len(kernel, source, &length);
```

For a document, the version is the current one; for a snapshot, it is the
version captured at acquisition and never changes. `cdin_text_len` is O(1).

### 6.3 Reading a range

The read API uses the usual two-call pattern when the required size is not
known:

```c
uint64_t needed = 0;
st = cdin_text_read(kernel, source, start, end, NULL, 0, &needed, NULL);
/* needed == end - start on success */

uint8_t *buffer = malloc((size_t)needed);
size_t got = 0;
st = cdin_text_read(kernel, source, start, end, buffer, (size_t)needed, &got, NULL);
```

Rules:

- `start <= end <= length`; otherwise `CDIN_OUT_OF_RANGE`.
- `end == start` is valid and reads zero bytes.
- `buf == NULL` is a size query; `out_len` must be non-NULL and receives
  `end - start`.
- If `buf` is non-NULL and `capacity < end - start`, the call returns
  `CDIN_BUFFER_TOO_SMALL`, copies nothing, and sets `*out_len` to the required
  size.
- `flags` is optional. It reports the position flags described in 6.5: whether
  `start` or `end` falls inside a multi-byte UTF-8 sequence. This is
  informational; the bytes are always copied exactly.
- `flags` is written on success, including a size query.

The bytes are copied exactly. NUL bytes, invalid UTF-8, BOMs, and mixed line
endings are neither removed nor rewritten.

### 6.4 Reading the whole document

`cdin_text_serialize` is the whole-document form:

```c
uint64_t length = 0;
cdin_text_serialize(kernel, source, NULL, 0, &length);
uint8_t *data = malloc((size_t)length);
st = cdin_text_serialize(kernel, source, data, (size_t)length, &length);
```

The rules are the same as `cdin_text_read`, with `start = 0` and
`end = length`. On `CDIN_BUFFER_TOO_SMALL`, `*out_len` is the required size.

### 6.5 Checking a position

```c
st = cdin_text_check_position(kernel, source, version, offset);
```

This is the authoritative test for an edit position:

- the version check happens first; a stale `version` returns
  `CDIN_STALE_VERSION` without examining `offset`;
- `offset > length` returns `CDIN_OUT_OF_RANGE`;
- `offset == length` is valid;
- a position strictly inside a well-formed multi-byte UTF-8 sequence returns
  `CDIN_INVALID_POSITION`;
- every raw byte is a valid position.

`cdin_text_read` reports the same mid-sequence information for the boundaries
of a range in its `flags` output: `CDIN_READ_STARTS_MID_SEQUENCE` and
`CDIN_READ_ENDS_MID_SEQUENCE`. These flags describe the range, not the
document, and are written only on success. The conversion functions in section
11 return `CDIN_CONV_TOUCHED_RAW` for a different purpose: to say that raw
bytes were crossed while converting between offsets and code-point indices.

`cdin_text_region_at` reports the maximal run of bytes around an offset that
all belong to the same class:

```c
uint64_t region_start = 0, region_end = 0;
int is_raw = 0;
st = cdin_text_region_at(kernel, source, offset, &region_start, &region_end, &is_raw);
```

`offset` must be less than the length. `is_raw == 0` means the region consists
of well-formed UTF-8 sequences; `is_raw != 0` means raw bytes. This is useful
for displaying damaged input byte by byte.

## 7. Editing safely

### 7.1 The replace request

`cdin_doc_replace` replaces `[start, end)` with `bytes`:

```c
typedef struct cdin_replace_request {
    uint32_t struct_size;
    uint32_t flags;               /* must be 0 */
    uint64_t expected_version;    /* CDIN_ANY_VERSION disables the check */
    uint64_t start, end;          /* half-open byte range */
    const uint8_t *bytes;
    size_t len;
    uint64_t coalesce_key;        /* 0 disables coalescing */
    const uint8_t *blob_before;
    size_t blob_before_len;
    const uint8_t *blob_after;
    size_t blob_after_len;
} cdin_replace_request;
```

- `start <= end <= length`; otherwise the call fails with
  `CDIN_OUT_OF_RANGE` or `CDIN_INVALID_ARGUMENT`.
- `start` and `end` must be valid positions (not inside a multi-byte
  sequence); otherwise `CDIN_INVALID_POSITION`.
- `bytes` must be non-NULL when `len > 0`. The bytes are copied.
- `len > max_edit_size` fails with `CDIN_LIMIT_EXCEEDED` and
  `details.limit_id == CDIN_LIMIT_EDIT_SIZE`.
- If `start + len - (end - start)` would exceed `max_document_size`, the call
  fails with `CDIN_LIMIT_DOCUMENT_SIZE`.
- `expected_version` is checked after the range and size checks.
- `blob_before` and `blob_after` are optional caller state; see section 8.4.
- `flags` must be `0`; any other value returns `CDIN_UNSUPPORTED`.

### 7.2 Order of checks

The order matters when several things are wrong. `cdin_doc_replace` checks:

1. `kernel`, `out`, and `out->struct_size`;
2. the document handle;
3. `req`, `req->struct_size`, and flags;
4. `start > end`;
5. the range against the current length and the UTF-8 positions;
6. `len` against `max_edit_size` and the resulting length against
   `max_document_size`;
7. blob sizes against `max_blob_size`;
8. whether an active transaction blocks the edit (`CDIN_BUSY`);
9. the no-op case;
10. `expected_version` and then acceptance (sequence number).

A request rejected at step 1-8 changes nothing and consumes no sequence number.
A request that passes step 10 but fails while allocating memory consumes its
sequence number and leaves content and version unchanged; `accepted_seq` is set
in that case.

The batch, undo, redo, and reload calls have the same shape; their exact
preconditions are listed with each call.

### 7.3 The edit result

```c
typedef struct cdin_edit_result {
    uint32_t struct_size;
    uint32_t edit_count;
    cdin_version new_version;
    uint64_t accepted_seq;
    uint64_t start, removed, inserted;
    uint64_t new_length;
    uint64_t blob_len;
    cdin_error_details details;
} cdin_edit_result;
```

| Field | Meaning |
|---|---|
| `edit_count` | Number of primitive edits: 1 for replace; `count` for a batch or committed transaction; the unit's edit count for undo/redo; 0 for a no-op. |
| `new_version` | Version after the call; unchanged for a no-op or a failure. |
| `accepted_seq` | Sequence number if the request was accepted; 0 otherwise. Non-zero does not guarantee that the content changed. |
| `start`, `removed`, `inserted` | Coordinates in the previous version. Valid only when `edit_count == 1`; otherwise 0. For undo/redo these describe the inverse operation. |
| `new_length` | Document length after the operation. |
| `blob_len` | Size of the caller blob associated with the history unit for undo/redo; 0 for replace, batch, reload, and transaction commit. |
| `details` | Failure detail; meaningful only for the statuses documented in section 15. |

`start`, `removed`, and `inserted` are not written for multi-edit operations.
A consumer that needs every primitive edit should subscribe to notifications
(section 13); the event carries up to eight edit descriptions and a truncation
flag.

### 7.4 No-op edits

A replace is a no-op when `start == end` and `len == 0`. A batch is a no-op
when its count is zero or every entry is such an empty edit. A committed
transaction with no tentative edits is also a no-op. In all three cases:

- the call returns `CDIN_OK`;
- `edit_count` is `0`;
- `new_version` is the unchanged current version;
- `accepted_seq` is `0`;
- no history unit is created, no blob is stored, and no notification is sent;
- the version check is skipped, so a stale `expected_version` does not produce
  an error for a no-op.

Code that relies on the version check for ordering must avoid submitting
no-ops, or treat `edit_count == 0` as "nothing happened".

### 7.5 Batch edits

`cdin_doc_replace_batch` applies up to `CDIN_MAX_BATCH_EDITS_HARD` (65536)
non-overlapping replacements as one undoable unit and one notification:

```c
cdin_batch_request req;
memset(&req, 0, sizeof req);
req.struct_size = (uint32_t)sizeof req;
req.expected_version = version;
req.count = 2;
req.starts = (const uint64_t[]){ 2, 10 };
req.ends = (const uint64_t[]){ 4, 12 };
req.bytes = (const uint8_t *const[]){ (const uint8_t *)"AB", NULL };
req.lens = (const size_t[]){ 2, 0 };
```

Rules:

- the ranges must be sorted ascending and must not overlap; adjacent ranges
  (`starts[i] == ends[i-1]`) are allowed;
- each `start <= end`; `ends[count-1] <= length`;
- every boundary must be a valid position;
- `bytes[i]` may be `NULL` only when `lens[i] == 0`;
- the total inserted bytes must not exceed `max_edit_size`;
- the simulated document length at every intermediate step must not exceed
  `max_document_size`; a batch whose final length fits but whose intermediate
  length does not is still rejected;
- `coalesce_key` must be `0`; `blob_before` and `blob_after` apply to the whole
  unit;
- the edits are applied from the last to the first, so earlier offsets remain
  valid while the later ones are replaced.

The whole batch is one history unit and one notification with
`kind == CDIN_NOTIF_BATCH`. The edit descriptions in that notification are
stored in application order (last range first).

### 7.6 Insert, delete, and replace

```c
/* Insert "X" at byte 3. */
req.start = 3;
req.end = 3;
req.bytes = (const uint8_t *)"X";
req.len = 1;

/* Delete bytes [5, 9). */
req.start = 5;
req.end = 9;
req.bytes = NULL;
req.len = 0;

/* Replace [0, 2) with "AB". */
req.start = 0;
req.end = 2;
req.bytes = (const uint8_t *)"AB";
req.len = 2;
```

To append, use `start = end = length`. To clear the document, use
`start = 0`, `end = length`, `len = 0`.

## 8. Undo, redo, and saved state

### 8.1 Undo and redo

```c
cdin_history_request req;
memset(&req, 0, sizeof req);
req.struct_size = (uint32_t)sizeof req;
req.expected_version = version;

cdin_edit_result result;
memset(&result, 0, sizeof result);
result.struct_size = (uint32_t)sizeof result;
st = cdin_doc_undo(kernel, doc, &req, &result);
```

`cdin_doc_undo` reverts the most recent applied history unit;
`cdin_doc_redo` re-applies the most recently undone unit. Both:

- require no active transaction (`CDIN_BUSY` otherwise);
- fail with `CDIN_INVALID_STATE` when there is nothing to undo or redo;
- check `expected_version`;
- increment the version by one;
- clear the coalescing chain;
- emit `UNDO` or `REDO`.

`result.edit_count` is the number of primitive edits in the unit. For a
batch or a transaction it can be greater than one; in that case `start`,
`removed`, and `inserted` are `0`, and the notification carries the individual
edits. For a one-edit unit the result describes the inverse operation (undo) or
the forward operation (redo).

If an undo or redo would move history past a unit that was evicted, the marker
is lost and the document stays dirty; see 8.5.

### 8.2 History units

One history unit is created by:

- one `cdin_doc_replace`;
- one `cdin_doc_replace_batch`;
- one committed transaction (all its edits in one unit);
- one coalesced run of replaces.

A new edit after an undo discards all redo units. The discarded units are not
counted in `evicted_total` and produce no notification.

`max_undo_memory` caps the accounted history memory per document. When a commit
pushes the total over the cap, the kernel discards the oldest units until the
total is at or below the cap, but always keeps the newest unit even if that
unit alone exceeds the cap. Each discard increments `evicted_total` and,
after the edit event, emits `CDIN_NOTIF_HISTORY_EVICTED` with the number of
units discarded in that commit. The saved-state marker is lost if its unit is
discarded.

### 8.3 Coalescing typing bursts

`cdin_replace_request.coalesce_key` groups consecutive replaces into one undo
unit:

- `0` disables coalescing;
- a non-zero key merges the new edit into the immediately preceding unit if
  that unit has the same key, is still the newest applied unit, and there is
  no redo tail;
- the merged unit keeps the `blob_before` of its first edit and the
  `blob_after` of its latest edit;
- undo/redo, `mark_clean`, reload, history eviction, or a different key ends
  the run.

Use a key derived from the editing session and the insertion point, for
example a hash of the cursor identity, so separate cursors do not merge. A
single undo then removes the whole burst.

Coalescing is not available for batches (the key must be zero) and does not
apply inside a transaction (the transaction is already one unit).

### 8.4 Caller blobs

A blob is opaque caller state stored with a history unit. The library never
interprets it. The typical use is an editor cursor or selection snapshot that
must be restored with the text:

```c
req.blob_before = before_bytes;   /* state before this edit */
req.blob_before_len = before_len;
req.blob_after = after_bytes;     /* state after this edit */
req.blob_after_len = after_len;
```

- Each blob is limited to `max_blob_size`; a larger blob returns
  `CDIN_LIMIT_EXCEEDED` with `details.limit_id == CDIN_LIMIT_BLOB_SIZE`.
- The bytes are copied immediately; the caller may reuse the buffer.
- Undo returns `blob_before`; redo returns `blob_after`.
- In a coalesced run, `blob_before` comes from the first edit and `blob_after`
  from the latest edit; intermediate blobs are dropped.

To read a blob from an undo or redo, use the same request:

```c
uint8_t blob[64];
req.blob_out = blob;
req.blob_capacity = sizeof blob;
st = cdin_doc_undo(kernel, doc, &req, &result);
if (st == CDIN_BUFFER_TOO_SMALL) {
    /* result.details.required is the blob size; call again with a larger buffer */
}
if (st == CDIN_OK) {
    size_t blob_len = (size_t)result.blob_len;
}
```

`blob_out` may be `NULL`; the blob is then not copied and `blob_len` still
reports its size. A too-small buffer is detected before the version check and
before acceptance, so it does not consume a sequence number or change history.
`CDIN_BUFFER_TOO_SMALL` sets `details.required` to the exact size.

### 8.5 Saved state

A document has a clean marker: the history position that was last marked as
saved. The initial document is clean. It becomes dirty when an edit moves away
from the marker, and clean again when undo or redo returns exactly to the
marker.

```c
cdin_doc_mark_clean(kernel, doc);   /* "saved" */

int clean = 0;
cdin_doc_is_clean(kernel, doc, &clean);
```

`cdin_doc_mark_clean`:

- marks the current history position clean;
- clears the lost-marker flag;
- clears the coalescing chain;
- consumes a sequence number;
- emits `CDIN_NOTIF_SAVED_STATE_CHANGED` only if the document was dirty;
- does not change the version and does not create a history unit.

The marker is lost when its history unit is evicted or discarded with a redo
tail. A document with a lost marker stays dirty until the next `mark_clean` or
`cdin_doc_reload`.

Only transitions dirty -> clean produce `SAVED_STATE_CHANGED`. An edit that
makes the document dirty does not emit it; poll `cdin_doc_is_clean` after
changes if the application needs to update a title bar. `cdin_doc_reload`
always results in a clean document and emits `SAVED_STATE_CHANGED` if it was
dirty before.

## 9. Transactions

A transaction is a private tentative copy of the document. Edits are applied
to the copy; the document itself does not change until commit. Beginning a
transaction is O(1) because the tentative copy shares the content until the
first edit (copy-on-write).

### 9.1 Begin

```c
cdin_txn_begin_request begin;
memset(&begin, 0, sizeof begin);
begin.struct_size = (uint32_t)sizeof begin;
begin.expected_version = version;

cdin_txn txn = 0;
st = cdin_txn_begin(kernel, doc, &begin, &txn);
```

- Only one transaction may be active per document; a second begin returns
  `CDIN_INVALID_STATE`.
- The begin is an accepted request and takes a sequence number.
- The document version does not change at begin.
- After shutdown, begin returns `CDIN_SHUTDOWN`.

### 9.2 Editing inside a transaction

`cdin_txn_replace` takes the same `cdin_replace_request` as
`cdin_doc_replace`, with three differences:

- `expected_version` must be `CDIN_ANY_VERSION`; the transaction already froze
  the version it started from;
- `coalesce_key` must be `0`;
- `blob_before`, `blob_after`, and their lengths must be `0`. Blobs belong to
  the commit, not to individual tentative edits.

The request is validated against the tentative content, so offsets refer to the
text after previous transaction edits. Each edit is limited by `max_edit_size`;
the accumulated tentative memory is limited by `max_undo_memory` and returns
`CDIN_LIMIT_EXCEEDED` with `details.limit_id == CDIN_LIMIT_TXN_MEMORY`.
`cdin_txn_replace` does not take a sequence number and does not bump the
document version. On success, `result.new_version` is the document version
(still unchanged) and `result.new_length` is the tentative length.

Read the tentative text with:

```c
uint64_t len = 0;
cdin_txn_len(kernel, txn, &len);

uint8_t *buf = malloc((size_t)len);
size_t got = 0;
cdin_txn_read(kernel, txn, 0, len, buf, (size_t)len, &got);
```

`cdin_txn_read` uses the same size-query and `CDIN_BUFFER_TOO_SMALL` rules as
`cdin_text_read`. Reading the document itself still shows the committed
content.

### 9.3 Commit

```c
cdin_txn_commit_request commit;
memset(&commit, 0, sizeof commit);
commit.struct_size = (uint32_t)sizeof commit;
commit.blob_before = before;
commit.blob_before_len = before_len;
commit.blob_after = after;
commit.blob_after_len = after_len;

cdin_edit_result result;
memset(&result, 0, sizeof result);
result.struct_size = (uint32_t)sizeof result;
st = cdin_txn_commit(kernel, txn, &commit, &result);
```

Commit publishes all tentative edits as one atomic document mutation:

- the transaction's edits become one history unit;
- the version increments by one;
- subscribers receive one `TRANSACTION` event containing the primitive edits
  (up to eight stored, with `edits_truncated` set if there were more);
- the commit takes a sequence number;
- the transaction handle is invalid after the call.

If the tentative content is identical to the committed content (no edits, or
only no-ops), commit returns `CDIN_OK` with `edit_count == 0`, does not change
the version, and does not consume a sequence number.

Commit is prepared fully before content changes. If an allocation fails, the
document and its history are unchanged and the transaction is rolled back and
invalidated. A failed commit is not retryable; begin a new transaction.

### 9.4 Abort

`cdin_txn_abort(kernel, txn)` discards the tentative content and invalidates
the handle. The document does not change, no sequence number is consumed, and
no notification is sent. Aborting after commit or abort returns
`CDIN_INVALID_HANDLE`.

### 9.5 Interaction rules

While a transaction is active on a document:

| Operation on that document | Result |
|---|---|
| `cdin_doc_replace`, `cdin_doc_replace_batch` | `CDIN_BUSY` |
| `cdin_doc_undo`, `cdin_doc_redo` | `CDIN_BUSY` |
| `cdin_doc_reload` | `CDIN_BUSY` |
| `cdin_txn_begin` | `CDIN_INVALID_STATE` |
| `cdin_doc_close` | Allowed; aborts the transaction |
| `cdin_kernel_shutdown` | Allowed; aborts the transaction |
| `cdin_doc_mark_clean`, `cdin_doc_set_metadata` | Allowed; they refer to the committed document |
| `cdin_snapshot_acquire` | Allowed; captures the committed content and version |
| `cdin_notif_subscribe` | Allowed |
| All reads and searches | Allowed; see committed content |

The transaction object is not a snapshot: it can change while the transaction
is open, and it is not visible through `cdin_source`.

## 10. Snapshots

A snapshot is an immutable view of one document version:

```c
cdin_version snapshot_version = 0;
cdin_snapshot snapshot = 0;
st = cdin_snapshot_acquire(kernel, doc, &snapshot_version, &snapshot);

/* Read or search through cdin_src_snap(snapshot). */

cdin_snapshot_release(kernel, snapshot);
```

Properties:

- `snapshot_version` is optional and receives the captured version.
- The captured content never changes, even if the document is edited,
  reloaded, or closed.
- A search on a snapshot reports the snapshot's version and is not disturbed
  by concurrent document edits.
- The number of live snapshots per document is limited by
  `max_snapshots_per_doc`; exceeding it returns `CDIN_LIMIT_EXCEEDED` with
  `details` unavailable (there is no edit result).
- `cdin_snapshot_acquire` is rejected after kernel shutdown
  (`CDIN_SHUTDOWN`); `cdin_snapshot_release` still works.
- Releasing a snapshot invalidates its handle; a second release or any use
  returns `CDIN_INVALID_HANDLE`.
- A snapshot does not receive notifications.

Snapshots keep the underlying content alive after `cdin_doc_close`. They are
freed when released or when the kernel is destroyed. Destroying a kernel with
live snapshots logs a warning through the log sink.

## 11. The text model

### 11.1 Bytes are the storage

A document stores the exact bytes passed to `cdin_doc_create`, `cdin_doc_replace`,
`cdin_doc_replace_batch`, `cdin_doc_reload`, and `cdin_txn_commit`. It does not:

- validate or repair UTF-8;
- remove a BOM;
- convert CRLF to LF or the reverse;
- stop at a NUL byte;
- normalize Unicode;
- add or remove a trailing newline.

A document may contain any byte value, including NUL and byte sequences that
are not valid UTF-8. The editing, undo, snapshot, and notification paths treat
the content as bytes. UTF-8 only matters to the text helpers and to literal
search.

### 11.2 Valid positions and raw bytes

UTF-8 validation is local and does not reject the document. Each byte is
classified as part of a well-formed sequence or as *raw*. A position
`offset` in `[0, length]` is a **valid edit position** when it is not strictly
inside a well-formed multi-byte sequence. A raw byte is always a valid
position, and a boundary between two raw bytes is valid.

For example, in the bytes `61 C3 A9 62` (`a`, U+00E9, `b`):

| Offset | Valid edit position? | Reason |
|---|---|---|
| 0, 1, 3, 4 | yes | boundaries between code points |
| 2 | no | inside the two-byte sequence `C3 A9` |

In `61 FF 62`, every offset is valid because `FF` is raw.

The distinction matters only for edits and for the code-point conversions.
Reads, searches, and line operations never reject content because of it.

### 11.3 Code points

A *unit* is one well-formed UTF-8 sequence or one raw byte. The code-point
index of a byte offset is the number of units before it. This means a raw byte
counts as one unit, not as a Unicode replacement character; no data is lost.

```c
uint64_t index = 0;
uint32_t flags = 0;
st = cdin_text_byte_to_codepoint(kernel, source, offset, &index, &flags);
if (flags & CDIN_CONV_TOUCHED_RAW) {
    /* the prefix contains bytes that are not valid UTF-8 */
}
```

- `offset` must be a valid position; otherwise `CDIN_INVALID_POSITION`.
- `offset == length` returns the number of units.
- `flags` is optional and receives `CDIN_CONV_TOUCHED_RAW` when raw bytes were
  crossed in the prefix.

The inverse:

```c
uint64_t offset = 0;
uint32_t flags = 0;
st = cdin_text_codepoint_to_byte(kernel, source, index, &offset, &flags);
```

- an index past the last unit returns `CDIN_OUT_OF_RANGE`;
- an index equal to the unit count returns `length`;
- flags have the same meaning.

Boundary helpers:

```c
uint64_t end_of_unit = 0, start_of_unit = 0;
cdin_text_codepoint_next(kernel, source, offset, &end_of_unit);
cdin_text_codepoint_prev(kernel, source, offset, &start_of_unit);
```

`next` returns the end of the unit containing `offset`; `prev` returns the start
of the unit containing `offset - 1`. Both accept an offset inside a multi-byte
sequence and snap to the surrounding unit. `next` requires
`offset < length`; `prev` requires `0 < offset <= length`; anything else is
`CDIN_OUT_OF_RANGE`.

These conversions walk the text from the beginning, so converting an offset is
O(offset). There is no persistent code-point index in ABI 1.0.0.

### 11.4 Grapheme clusters

Grapheme boundaries are extended grapheme clusters according to Unicode 15.1,
UAX #29 rules GB1-GB13 plus GB9c. A raw byte is always its own cluster; no
boundary rule joins anything to it.

```c
uint64_t cluster_end = 0, cluster_start = 0;
cdin_text_grapheme_next(kernel, source, offset, &cluster_end);
cdin_text_grapheme_prev(kernel, source, offset, &cluster_start);
```

- `next` returns the end of the cluster containing `offset`;
- `prev` returns the start of the cluster containing `offset - 1`;
- the range rules are the same as for code points;
- combining marks, emoji modifiers, and ZWJ sequences are kept together
  according to the Unicode rules.

The library does not compute display columns or terminal widths. An editor
that needs a visual column must use its font metrics and justify that decision
itself.

### 11.5 Lines and line endings

A line terminator is one of:

| Style | Bytes | Counted as |
|---|---|---|
| LF | `0A` | one `lf` |
| CRLF | `0D 0A` | one `crlf` |
| CR | `0D` not followed by `0A` | one `cr` |

A CRLF is one terminator, never a CR plus an LF. The library never changes the
style.

```c
uint64_t lines = 0;
cdin_text_line_count(kernel, source, &lines);

uint64_t start = 0;
cdin_text_line_start(kernel, source, 3, &start);

uint64_t line = 0, column = 0;
cdin_text_line_from_offset(kernel, source, offset, &line, &column);
```

- `line_count` is the number of terminators plus one. The count includes the
  empty final line after a trailing terminator, so an empty document has one
  line and `"a\n"` has two.
- `line_start` takes a zero-based line index. An index at or beyond
  `line_count` returns `CDIN_OUT_OF_RANGE`.
- `line_from_offset` accepts `offset <= length`; `column` is the byte offset
  within the line, not a character or display column. `offset == length` maps
  to the last line.
- The first line query may build and cache a line index; an edit invalidates
  it. Snapshots keep the index of their immutable block.

End-of-line statistics:

```c
cdin_eol_info eol;
memset(&eol, 0, sizeof eol);
eol.struct_size = (uint32_t)sizeof eol;
cdin_text_eol_info(kernel, source, &eol);
```

| Field | Meaning |
|---|---|
| `lf`, `crlf`, `cr` | Number of terminators of each style. |
| `final_line_unterminated` | Non-zero when the text is non-empty and its last byte is not `0A` or `0D`. |
| `mixed` | Non-zero when more than one of the three styles is present. |

## 12. Search

### 12.1 Modes

`cdin_search` supports two modes in ABI 1.0.0:

| Mode | Matches |
|---|---|
| `CDIN_SEARCH_LITERAL` | A UTF-8 pattern in valid UTF-8 regions. Case-insensitive matching and newline normalization are optional. |
| `CDIN_SEARCH_BYTES` | An exact byte pattern anywhere, including raw bytes and NUL. |
| `CDIN_SEARCH_REGEX` | Not supported in 1.0.0; returns `CDIN_UNSUPPORTED`. |

### 12.2 The request

```c
typedef struct cdin_search_request {
    uint32_t struct_size;
    uint32_t flags;
    cdin_source src;
    cdin_version expected_version;
    const uint8_t *pattern;
    size_t pattern_len;
    cdin_search_mode mode;
    cdin_search_direction direction;
    uint64_t from;
    uint64_t bound;
    uint64_t limit;
    uint64_t step_limit;
    const uint32_t *cancel;
    uint64_t *ranges;              /* out: [start,end) pairs */
    size_t ranges_capacity;        /* in pairs */
} cdin_search_request;
```

| Field | Meaning |
|---|---|
| `mode` | `CDIN_SEARCH_LITERAL`, `CDIN_SEARCH_BYTES`, or `CDIN_SEARCH_REGEX`. |
| `direction` | `CDIN_SEARCH_FORWARD` or `CDIN_SEARCH_BACKWARD`. |
| `flags` | Any combination of `CDIN_SEARCH_CASE_INSENSITIVE`, `CDIN_SEARCH_ANY_NEWLINE`, and `CDIN_SEARCH_WANT_PARTIAL`. |
| `expected_version` | Version to search; `CDIN_ANY_VERSION` disables the check. |
| `src` | Document or snapshot. |
| `pattern`, `pattern_len` | Pattern bytes; never NUL-terminated. |
| `from`, `bound` | Search geometry; see 12.3. |
| `limit` | Maximum matches to return; must be at least 1. |
| `ranges`, `ranges_capacity` | Caller array of byte-offset pairs, `[start, end)`; `ranges_capacity` counts pairs and `limit` may not exceed it. |
| `step_limit` | Work budget; 0 selects the kernel's `default_search_steps`. |
| `cancel` | Optional pointer to a `uint32_t`; a non-zero value cancels. |

`CDIN_SEARCH_CASE_INSENSITIVE` and `CDIN_SEARCH_ANY_NEWLINE` are valid only in
literal mode; using them with bytes mode returns `CDIN_INVALID_ARGUMENT`.
Unknown flag bits return `CDIN_UNSUPPORTED`. Unknown mode or direction values
return `CDIN_INVALID_ARGUMENT`.

### 12.3 Geometry

Forward search:

- candidates are examined starting at `from`;
- a match is accepted when `match_start >= from` and
  `match_end <= bound`;
- after a match, scanning continues at `match_end`, so matches never overlap.

Backward search:

- candidates are examined starting at `from` and moving toward lower offsets;
- a match is accepted when `match_start <= from` and
  `match_end <= bound`;
- matches are returned from the rightmost to the leftmost and never overlap.

`bound` larger than the source is rejected. Use `UINT64_MAX` to mean the end
of the source. Both `from` and `bound` must be at most the source length.
For a forward search over the whole document:

```c
req.from = 0;
req.bound = UINT64_MAX;
```

For a backward search from a cursor at byte `p` over everything before it:

```c
req.from = p;
req.bound = UINT64_MAX;
```

For a literal search, `from` and `bound` must also be valid positions (not
inside a multi-byte sequence); otherwise `CDIN_INVALID_POSITION`. Raw positions
are valid.

`bound < from` is allowed and produces no matches.

### 12.4 The result

```c
typedef struct cdin_search_result {
    uint32_t struct_size;
    cdin_version version;
    uint64_t count;
    int truncated;
    int partial;
    int skipped_raw;
    uint64_t next_from;
    uint64_t next_bound;
    int exhausted;
    uint64_t steps;
    cdin_error_details details;
} cdin_search_result;
```

Matches are written to `req.ranges` as `[start, end)` byte-offset pairs of the
scanned version; `req.ranges` is `2 * count` `uint64_t` values. `count` never
exceeds `min(limit, ranges_capacity)`.

| Field | Meaning |
|---|---|
| `count` | Matches written to `req.ranges`. |
| `version` | Version of the source that was scanned. Written on success and on a partial result. |
| `next_from`, `next_bound` | Where to continue; see 12.5. |
| `steps` | Work units consumed; approximately the number of candidate offsets examined. |
| `truncated` | `count == limit`; more matches may exist. |
| `partial` | A step limit or cancellation stopped the scan and `WANT_PARTIAL` was set. |
| `exhausted` | The scan reached the end of the requested range without hitting the limit. |
| `skipped_raw` | A literal scan crossed invalid UTF-8 in the scanned range. Set only for a completed scan. |
| `details` | Failure information. |

### 12.5 Pagination

Pass the input fields back on the next call:

```c
enum { RANGE_COUNT = 64 };
uint64_t ranges[2 * RANGE_COUNT];

for (;;) {
    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;

    req.limit = RANGE_COUNT;
    req.ranges = ranges;
    req.ranges_capacity = RANGE_COUNT;

    st = cdin_search(kernel, &req, &result);
    if (st != CDIN_OK) break;

    use(ranges, result.count);
    if (result.exhausted) break;

    req.from = result.next_from;
    req.bound = result.next_bound;
}
```

- forward: `next_from` is the end of the last match (or the original `from`
  when there was no match), and `next_bound` is the original `bound`;
- backward: `next_from` is the start of the last returned match (or `0` when
  the scan reached the beginning), and `next_bound` is the start of the last
  returned match, so the next call cannot return an overlapping match;
- after an exhausted search, `next_from` and `next_bound` are still written;
  do not call again unless you change the geometry.

If `count == limit`, `truncated` is set even when no further match exists. The
next call returns `count == 0` and `exhausted == 1`.

### 12.6 Limits, cancellation, and partial results

`step_limit` bounds the work. When the budget is exhausted, the call returns
`CDIN_LIMIT_EXCEEDED` with `details.limit_id == CDIN_LIMIT_SEARCH_STEPS`.

`cancel` points to a `uint32_t`. A non-zero value stops the scan and returns
`CDIN_CANCELLED`. The pointer may be `NULL`; when non-NULL, it must stay valid
for the duration of the call. The check is cheap and periodic, not synchronous
with the write; a cancellation is observed at the next check. The search reads
it with relaxed atomic semantics, so no additional synchronization is required
for a flag that is only ever changed from 0 to non-zero.

Without `CDIN_SEARCH_WANT_PARTIAL`, a step-limit or cancellation failure writes
only `details`; `count`, `version`, and the continuation fields are not valid
for the caller. With `CDIN_SEARCH_WANT_PARTIAL`:

- the matches found so far are written to `ranges`;
- `partial == 1`, `truncated == 0`, `exhausted == 0`;
- `version`, `steps`, `next_from`, and `next_bound` are valid;
- the status is still `CDIN_LIMIT_EXCEEDED` or `CDIN_CANCELLED`.

### 12.7 Literal matching rules

- The pattern must be valid UTF-8; otherwise `CDIN_PATTERN_INVALID`.
- The pattern is matched against well-formed UTF-8 sequences only. A raw byte
  in the document never matches any literal pattern.
- `CDIN_SEARCH_CASE_INSENSITIVE` uses simple case folding, code point by code
  point. Simple folding maps one code point to at most one code point: unlike
  full folding, U+00DF (sharp s) does not match the two-character sequence
  `SS`, and no normalization is applied.
- `CDIN_SEARCH_ANY_NEWLINE` treats LF in the pattern as matching LF, CRLF, or
  a lone CR. A CRLF in the pattern is normalized to LF for the comparison.
- A match is reported at a valid UTF-8 boundary.
- If a completed literal scan crossed bytes that are not valid UTF-8,
  `skipped_raw == 1`. A scan stopped by the limit or by cancellation does not
  report this flag.

### 12.8 Bytes matching rules

- Every byte sequence matches, including NUL bytes and invalid UTF-8.
- Forward and backward geometry and non-overlap rules are the same as for
  literal mode.
- `CDIN_SEARCH_CASE_INSENSITIVE` and `CDIN_SEARCH_ANY_NEWLINE` are rejected
  with `CDIN_INVALID_ARGUMENT`.
- No UTF-8 position checks are applied to `from` or `bound`.

### 12.9 Failure precedence

`cdin_search` checks in this order:

1. `kernel`, `out`, `out->struct_size`, and `req`;
2. mode, direction, flags, `pattern`/`pattern_len`, `limit`, `ranges`, and
   `ranges_capacity`;
3. pattern validity in literal mode;
4. source resolution (handle validity);
5. `expected_version`;
6. `from` and `bound` against the source length;
7. literal positions;
8. pattern preparation allocations.

Argument errors therefore win over handle errors, and a stale version wins
over an out-of-range position. On failure, `details` is written where the
header documents it; other result fields are not valid.

## 13. Notifications

### 13.1 What a subscription gives you

A subscription is a bounded queue of change events for one document plus an
optional wakeup callback. The queue belongs to the subscriber, so the kernel
rarely blocks notifications are never lost silently and a mutation is never
delayed by how many consumers exist.

```c
cdin_subscribe_request sub;
memset(&sub, 0, sizeof sub);
sub.struct_size = (uint32_t)sizeof sub;
sub.wakeup = on_change;        /* optional */
sub.wakeup_ctx = &my_ctx;      /* optional */

cdin_version start_version = 0;
cdin_notif sub_handle = 0;
st = cdin_notif_subscribe(kernel, doc, &sub, &start_version, &sub_handle);
```

- `start_version` is the version at which the subscription begins. Events are
  queued only for changes after that version; nothing is replayed.
- Each document accepts at most `max_subscribers_per_doc` live subscriptions.
  Subscribe returns `CDIN_LIMIT_EXCEEDED` (without edit-result details) when
  the limit is reached.
- `flags` must be `0`; anything else returns `CDIN_UNSUPPORTED`.
- The subscription handle is invalidated by `cdin_notif_unsubscribe` and by
  destroying the kernel.
- The optional `wakeup` callback runs right after an event is queued, on the
  thread that performed the mutation, with the caller lock already released.
  It is a hint only; drain gives the authoritative queue contents. See 13.6.

### 13.2 Events

All events are `cdin_notif_event` values copied into the caller's buffer:

```c
typedef struct cdin_notif_event {
    uint32_t struct_size;
    cdin_notif_kind kind;
    cdin_doc doc;
    cdin_version from_version;
    cdin_version to_version;
    cdin_version current_version;
    uint32_t edit_count;
    uint32_t edits_stored;
    int edits_truncated;
    uint64_t evicted;
    int is_clean;
    cdin_edit_desc edits[CDIN_NOTIF_MAX_EDITS];
} cdin_notif_event;

typedef struct cdin_edit_desc {
    uint64_t start;
    uint64_t removed;
    uint64_t inserted;
} cdin_edit_desc;
```

| Kind (value) | Emitted by | Fields that matter |
|---|---|---|
| `CDIN_NOTIF_EDIT` (0) | one replace | `edit_count == 1`; `edits[0]`; `to_version = from_version + 1`; `current_version == to_version` |
| `CDIN_NOTIF_BATCH` (1) | replace_batch | `edit_count == count`, `edits_stored = min(edit_count, 8)`, in application order (last range first) |
| `CDIN_NOTIF_TRANSACTION` (2) | transaction commit | `edit_count == number of edits` committed at commit |
| `CDIN_NOTIF_UNDO` (3) | undo | `edits` are the inverse operation applied in reverse order |
| `CDIN_NOTIF_REDO` (4) | redo | `edits` describe the re-applied operation in forward order |
| `CDIN_NOTIF_RELOAD` (5) | doc_reload | `edit_count == 0`; full re-read at `current_version == to_version` |
| `CDIN_NOTIF_CLOSED` (6) | doc_close | `current_version` is the last version; always the final event queued for the subscription |
| `CDIN_NOTIF_HISTORY_EVICTED` (7) | history overflow (after the commit's edit event) | `evicted == units dropped in that commit |
| `CDIN_NOTIF_RESYNC` (8) | queue overflow | `current_version` is the version to re-read at; earlier queued details are lost |
| `CDIN_NOTIF_SAVED_STATE_CHANGED` (9) | doc_mark_clean or doc_reload going to clean | `is_clean == 1`; no version change |

`from_version`/`to_version`: edit-like events advance from_version by one
version. For `UNDO`/`REDO`, `from` is before and `to` after the inverse edit:
each event's `edit_count` describes the applied unit with coordinates in the
state seen by that edit (`start` in bytes, `removed`, `inserted`); apply the
ranges top-to-bottom reversed when replacing whole units; see 13.4.

### 13.3 Draining

```c
cdin_notif_event events[32];
size_t count = 0;
st = cdin_notif_drain(kernel, sub_handle, events, 32, &count);
```

- Events leave the queue in FIFO order; anything not drained stays queued.
- `max` may be `0` together with `events == NULL` to query the queue without
  copying; `count` reports the number of queued events? No: with `max == 0` no
  events are copied and `count` is `0`. To learn the queue depth, drain into a
  sufficiently large array or keep draining until `count == 0`.
- `max > 0` requires `events != NULL` (`CDIN_INVALID_ARGUMENT` otherwise).
- `count` is always written when the call returns `CDIN_OK`.

### 13.4 Applying recorded edits

An `EDIT` event describes one changed range in the coordinates of the state
right before that edit:

```c
/* from_version at [start, start+removed) was replaced by `inserted` bytes */
```

For batch, transaction, and coalesced edits, `edit_count` can exceed
`CDIN_NOTIF_MAX_EDITS (8)`; then `edits_truncated != 0`, `edits_stored == 8`
and the list is incomplete. Treat a truncated event like `RESYNC`: re-read the
document at `to_version` instead of replaying the partial list.

For `UNDO` events the listed edits are in the order the library applied them,
which is the reverse of the unit's original order with `removed` and
`inserted` swapped; for `REDO` they are in the unit's original order with the
forward sizes. Each description's `start` is in the coordinates of the state
in force immediately before that particular edit. In the common case of one
recorded edit per event you do not need this detail.

### 13.5 Closing and cleanup

`cdin_doc_close` emits `CLOSED` to every subscriber, then detaches them. The
subscription handle remains valid and drainable until `cdin_notif_unsubscribe`,
and after the `CLOSED` event no new events are queued for it. A final drain
that returns `count == 0` means the queue is empty.

`cdin_notif_unsubscribe` discards any queued events and frees the queue. The
handle is immediately invalid afterwards: both drain and unsubscribe then
return `CDIN_INVALID_HANDLE`. Unsubscribing a document-closed subscription is
required to fully release its memory.

### 13.6 The wakeup callback

`wakeup(ctx)` is invoked at most once per mutation per subscription, only when
at least one event was queued for that subscription by that mutation. Collapsed
`RESYNC` queues and version advances on an existing `RESYNC` also trigger the
wakeup. It is never called for drain, subscribe, unsubscribe, or reads, and it
runs even when the queue overflowed.

Keep the body minimal: set a flag, signal a condition variable, or write a
byte to a pipe, and let a worker thread drain. Do not destroy the kernel or the
document from inside the wakeup.

## 14. Threads

The library never creates threads and never blocks on I/O, so it imposes few
threading rules. All of them concern the single lock and the caller callbacks.

| Situation | Rule |
|---|---|
| No lock configured | Call the kernel from one thread at a time. There is no internal synchronization; concurrent calls corrupt state. |
| Lock vtable configured | Public API calls may run on different threads simultaneously and are serialized by the caller's lock. Handles can be used from any thread that respects the call-serialization. |
| `cdin_kernel_destroy` | Must be exclusive in both cases: no other call on the same kernel may run, even with locks. |
| `cdin_kernel_shutdown` | Serialized by the lock like any public call; it waits for in-flight calls to finish and then aborts still-pending transactions. |
| `cdin_search` | The scan releases the lock while reading the pinned bytes, so other threads can edit a different document (with a lock vtable) while one thread searches. |
| Allocator callbacks | Are invoked inside calls, while the caller's lock is held. They must not call the kernel back. |
| Log callback | Invoked without the lock held. |
| Wakeup callback | Invoked without the lock held, as noted in 13.1. |

A wakeup callback runs on whatever thread performed the mutation. Keep it
minimal: copy the event count or set a flag and wake a worker. Do not perform
file I/O or heavy parsing inside it; do not destroy the kernel from within it.

A lock vtable serializes all calls for an entire kernel, including calls on
different documents. That is the intended coarse granularity: correctness
first, per-document parallelism is a future optimization. Concurrent writers
on the same document remain safe in a different sense: the version check makes
a lost-update race fail with `CDIN_STALE_VERSION` rather than corrupt
history.

## 15. Status codes and failures

### 15.1 The complete status list

| Value | Status | Typical cause | What to do |
|---|---|---|---|
| 0 | `CDIN_OK` | Success | Continue. |
| 1 | `CDIN_INVALID_ARGUMENT` | NULL pointer, empty range violation, bad struct size or flags, value shape wrong | Fix the argument. |
| 2 | `CDIN_INVALID_HANDLE` | Handle closed, committed, released, unsubscribed, stale reuse, or from another kernel | Get a new handle. |
| 3 | `CDIN_STALE_VERSION` | `expected_version` does not match | Re-read the document at `details.current` and retry or merge. |
| 4 | `CDIN_OUT_OF_RANGE` | `start`/`end`/`from`/`bound`/offset outside `[0, length]`, besides `start > end` | Clamp or recompute using `details.length`. |
| 5 | `CDIN_INVALID_POSITION` | Position inside a multi-byte UTF-8 sequence | Snap with `cdin_text_*` boundary helpers. |
| 6 | `CDIN_INVALID_STATE` | Operation impossible in the current state: second transaction begin, undo with empty history, redo with nothing to redo | Check state first or treat as a user-level no-op. |
| 7 | `CDIN_BUSY` | Document locked by an active transaction during replace/batch/undo/redo/reload | Finish or abort the transaction. |
| 8 | `CDIN_LIMIT_EXCEEDED` | A documented limit was reached | See `details.limit_id` in 15.2 and the limits table. |
| 9 | `CDIN_NO_MEMORY` | Allocator returned NULL | Free memory or shrink the request; nothing was applied (see 15.3). |
| 10 | `CDIN_PATTERN_INVALID` | Literal search pattern is not valid UTF-8 | Fix the pattern. |
| 11 | `CDIN_CANCELLED` | Cancel flag became non-zero during search | Resubmit if desired. |
| 12 | `CDIN_SHUTDOWN` | Kernel has been shut down | Stop mutating this kernel or create a new one. |
| 13 | `CDIN_UNSUPPORTED` | Feature not in this ABI: regex mode, non-zero reserved `flags`, unknown `struct_size` extension | Match the documented shape. |
| 14 | `CDIN_BUFFER_TOO_SMALL` | Caller buffer too small | See the size in the buffer rule (out_len) or `details.required`. |
| 15 | `CDIN_VERSION_MISMATCH` | `abi_major` does not match at kernel creation | Rebuild against a header the library was compiled from. |
| 16 | `CDIN_INTERNAL` | Kernel invariant violation (a library bug) | Report with the call sequence; treat output as garbage. |

`cdin_status_name` returns a static string such as `"CDIN_OK"`. The string
lives for the whole process; never free it.

### 15.2 Error details struct

`details` is defined in the header as:

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

| Field | Meaning | When |
|---|---|---|
| `limit_id` | Which `CDIN_LIMIT_*` was exceeded | `CDIN_LIMIT_EXCEEDED` |
| `expected` | Version the caller sent | `CDIN_STALE_VERSION` on edit/undo/redo/reload/search (not `cdin_text_check_position`, which has no details) |
| `current` | Version the document has | Same as `expected` |
| `requested` | Offending value | `CDIN_OUT_OF_RANGE` or `CDIN_INVALID_POSITION` on edits and search |
| `length` | Document length at the call | Same as `requested` |
| `required` | Bytes needed | `CDIN_BUFFER_TOO_SMALL` with details (undo/redo blob); the `text_read` and `serialize` calls instead set `*out_len` |
| - | `details` unwritten on success | In edit results, it is zeroed; in search results, it retains caller values |

`details` is a value in the caller's `out` struct. The library zeroes it when
it prepares a result and fills fields only for failures.

### 15.3 What a failure guarantees

If a mutating call returns anything other than `CDIN_OK`:

- the document content and version are unchanged;
- the undo/redo history is unchanged;
- no notification is queued;
- transactions are unaffected on the document path: an active transaction and
  its tentative edits stay, but a *failed commit* is the exception: it rolls
  back and invalidates the transaction (see 9.3);
- the call can safely be retried with corrected arguments.

One nuance: a call that was *accepted* (passed the version check) but failed
while allocating memory still consumes a sequence number. In that case
`accepted_seq` is non-zero, but the document and version are untouched and no
events exist. This is documented in CONC-004; gaps in sequence numbers are
normal.

## 16. Limits

### 16.1 Configurable limits

| Limit | Default | Meaning |
|---|---|---|
| `max_documents` | 1024 | Live documents in one kernel |
| `max_document_size` | 1 GiB | Bytes in one document |
| `max_edit_size` | `max_document_size` | Inserted bytes per edit; also the inserted-bytes budget of a batch |
| `max_undo_memory` | 64 MiB | History bytes per document; also the cap on tentative transaction memory |
| `max_blob_size` | 4096 | Per caller blob (`before` and `after` are two blobs) |
| `max_snapshots_per_doc` | 16 | Live snapshots of one document |
| `max_subscribers_per_doc` | 16 | Live subscriptions of one document |
| `notif_queue_capacity` | 64 | Events queued per subscriber (clamped 4..65536) |
| `default_search_steps` | 2^32 | Search steps when a request passes 0 |

All values are per kernel; snapshots are counted per document. When the
pipeline is pushed to a limit it never blocks: the subscriber queue collapses
to `RESYNC` and the undo history forgets its oldest units.

### 16.2 Hard caps independent of configuration

| Limit | Value | Reason |
|---|---|---|
| Maximum edits in one batch | 65536 (`CDIN_MAX_BATCH_EDITS_HARD`) | Fixed request-struct layout |
| Maximum stored edit descriptions per event | 8 (`CDIN_NOTIF_MAX_EDITS`) | Event struct is fixed-size; overflow sets `edits_truncated` |
| Minimum notification queue capacity | 4 | `CLOSED` must always fit behind a collapse to `RESYNC` |
| Maximum notification queue capacity | 65536 | Bounded ring size regardless of configuration typos |
| Maximum subscribers per document | 64 (`CDIN_MAX_SUBSCRIBERS_HARD`) | Larger configured values are clamped down |
| Version counter | refuses to wrap (`CDIN_LIMIT_VERSION`) | `UINT64_MAX` is reserved for `CDIN_ANY_VERSION` |
| Sequence counter | refuses to wrap (`CDIN_LIMIT_SEQUENCE`) | monotonic, 64-bit |
| History units | only bounded by `max_undo_memory`; eviction never removes the newest unit | Keeps undo meaningful under any workload |

## 17. Recipes

Each recipe is a sequence of calls against one kernel `k`. Error handling is
omitted for brevity; wrap everything with the `CHECK` pattern from section 3.

### 17.1 Load a file, edit, save

```c
/* Read the file with the application's own I/O library. */
uint8_t *data = read_whole_file(path, &len);          /* your code */

cdin_doc doc = 0;
cdin_doc_create(k, data, len, &doc);
free(data);                        /* the kernel made its own copy */

/* ... edit with cdin_doc_replace; checkpoints with mark_clean ... */

uint64_t n = 0;
cdin_text_serialize(k, cdin_src_doc(doc), NULL, 0, &n);
uint8_t *out = malloc(n);
size_t got = 0;
cdin_text_serialize(k, cdin_src_doc(doc), out, n, &got);
write_whole_file(path, out, got);                     /* your code */
free(out);

cdin_doc_close(k, doc);
```

`write_whole_file` must write exactly `got` bytes; document bytes are not
NUL-terminated. Do not use `strlen` on them.

### 17.2 Replace every occurrence of a literal

```c
cdin_search_request sreq;
uint64_t ranges[2 * 256];
cdin_search_result sres;

/* one kernel call per page of at most 256 matches, bounded loop */
memset(&sreq, 0, sizeof sreq);
sreq.struct_size = (uint32_t)sizeof sreq;
sreq.src = cdin_src_doc(doc);
sreq.expected_version = CDIN_ANY_VERSION;
sreq.pattern = (const uint8_t *)"old";
sreq.pattern_len = 3;
sreq.mode = CDIN_SEARCH_LITERAL;
sreq.direction = CDIN_SEARCH_FORWARD;
sreq.from = 0;
sreq.bound = UINT64_MAX;

for (;;) {
    memset(&sres, 0, sizeof sres);
    sres.struct_size = (uint32_t)sizeof sres;
    sreq.limit = 256;
    sreq.ranges = ranges;
    sreq.ranges_capacity = 256;
    if (cdin_search(k, &sreq, &sres) != CDIN_OK) break;
    if (sres.count > 0) {
        /* collect a batch; choose a size fitting inside max_edit_size */
        submit_batch_of_replacements(doc, ranges, sres.count);
        break;   /* re-search after the batch: offsets moved */
    }
    if (sres.exhausted) break;
    sreq.from = sres.next_from;
    sreq.bound = sres.next_bound;
}
```

For a bulk rename, prefer the transaction form: begin a transaction, submit
one batch per page with `expected_version == CDIN_ANY_VERSION`, then commit
once. The reader story is simple: either all replacements are visible or
none.

### 17.3 Undo with editor state

```c
/* per edit: snapshot cursor positions as opaque bytes (your format) */
uint8_t blob_before[64], blob_after[64];
size_t before_len = save_cursors(blob_before);
size_t after_len = save_cursors_blob(req); /* after computing the edit */

req.blob_before = blob_before;
req.blob_before_len = before_len;
req.blob_after = blob_after;
req.blob_after_len = after_len;
cdin_doc_replace(k, doc, &req, &result);

/* on undo: */
cdin_history_request hreq;
uint8_t restored[64];
memset(&hreq, 0, sizeof hreq);
hreq.struct_size = (uint32_t)sizeof hreq;
hreq.expected_version = current_version;
hreq.blob_out = restored;
hreq.blob_capacity = sizeof restored;
cdin_doc_undo(k, doc, &hreq, &result);
restore_cursors(restored, result.blob_len);
```

### 17.4 Track changes from a worker thread

```c
/* main thread */
cdin_kernel *k;                       /* with lock vtable */
cdin_notif sub = 0;
cdin_subscribe_request sr;
memset(&sr, 0, sizeof sr);
sr.struct_size = (uint32_t)sizeof sr;
sr.wakeup = signal_worker;            /* sets a flag / condvar */
sr.wakeup_ctx = &ctx;
cdin_notif_subscribe(k, doc, &sr, NULL, &sub);

/* worker thread, when signalled */
cdin_notif_event ev[64];
size_t n = 0;
cdin_notif_drain(k, sub, ev, 64, &n);
for (size_t i = 0; i < n; i++) {
    switch (ev[i].kind) {
    case CDIN_NOTIF_RELOAD:
    case CDIN_NOTIF_RESYNC:
        resync_hard(ev[i].current_version);       /* full re-read */
        break;
    case CDIN_NOTIF_EDIT:
    case CDIN_NOTIF_BATCH:
    case CDIN_NOTIF_TRANSACTION:
    case CDIN_NOTIF_UNDO:
    case CDIN_NOTIF_REDO:
        apply_descriptions(&ev[i]);               /* replay range updates */
        break;
    case CDIN_NOTIF_CLOSED:
        close_tab();
        break;
    default:
        update_saved_flag(ev[i].is_clean);        /* saved-state, eviction */
        break;
    }
}
```

### 17.5 Show line and column for a cursor

```c
uint64_t line = 0, col = 0;
cdin_text_line_from_offset(k, cdin_src_doc(doc), cursor, &line, &col);
/* 1-based display: line + 1, col + 1; both are byte columns */
```

Use `cdin_text_grapheme_next` / `_prev` to move the cursor by visible
characters instead of bytes.

## 18. Edge cases and answers

**Can I store binary data?** Yes. Documents are bytes. Literal search treats
only the valid UTF-8 parts as text; `CDIN_SEARCH_BYTES` searches everything.

**What happens to a byte order mark?** Nothing: it is stored and returned as
is. Strip one before create if your application treats it as metadata.

**What about CRLF?** Preserved byte for byte; `cdin_text_line_*` counts CRLF
as one terminator; `cdin_text_eol_info.mixed` reports it; nothing is
normalized.

**Does undo work after reload?** No. Reload discards history, and the document
becomes clean.

**Can two writers race on one document?** With a lock vtable they are
serialized. The first accepted write succeeds; the other fails with
`CDIN_STALE_VERSION`, and its `details.current` is the newer version to
re-read.

**What is the synchronize cost of a no-op replace?** The call still validates
arguments, so it is not free; but it neither takes a sequence number, nor
bumps the version or emits events. A stale `expected_version` does not
penalize no-ops; the version check is skipped.

**How do I detect a stale handle after close?** Every use returns
`CDIN_INVALID_HANDLE`. There is no way to close twice profitably; the second
close of the same handle is an error, not a no-op.

**Does subscribe see past events?** No. Driven purely by changes after
subscription. To build initial state, read the document first and subscribe
afterwards; any change in between will show up as an event.

**What is RESYNC exactly?** The subscriber queue overflowed. Treat it as "the
document changed by more than I can express": re-read at `current_version`
and re-derive whatever the consumer tracks (including the saved-state flag).

**Can notifications describe a transaction?** Yes: the kind is
`CDIN_NOTIF_TRANSACTION`, the `edits[]` count is the tentative-edit count,
and each description gives the range in the coordinates of the state right
before that edit.

**A failed edit consumed a sequence but shows version unchanged. Is that OK?**
Yes. This is the documented CONC-004 semantics: acceptance reserves an order
position; application is separate. Retry freely after fixing the memory
pressure.

**Are handles portable across kernels?** No. A handle is meaningful only for
the kernel that issued it; using one on another kernel returns
`CDIN_INVALID_HANDLE`.

**What if the version or sequence reaches its maximum?** Both refuse to wrap:
further mutations fail with `CDIN_LIMIT_EXCEEDED` (`CDIN_LIMIT_VERSION` or
`CDIN_LIMIT_SEQUENCE` in `details.limit_id`).






