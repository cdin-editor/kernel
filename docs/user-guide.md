# User guide

This is the main guide for application developers using `cdin-kernel`. It explains what the engine owns, how a typical edit flows through the API, and how to handle text positions without corrupting data. Keep [`include/cdin/kernel.h`](../include/cdin/kernel.h) open as the exact C declaration reference.

## 1. The mental model

The engine has four main concepts:

- **Kernel (`cdin_kernel *`)** — an owner and configuration boundary for documents, transactions, snapshots, and subscriptions. Each application should normally create one kernel for one working session or service instance.
- **Document (`cdin_doc`)** — a mutable byte sequence with a version number, undo/redo history, metadata, a saved-state marker, and optional notification subscriptions.
- **Snapshot (`cdin_snapshot`)** — a read-only view of a specific document version. A snapshot remains unchanged when the live document is edited.
- **Version (`cdin_version`)** — a number that lets callers say “apply this edit only if the document is still at the version I read.” It is not a timestamp.

The kernel owns allocated data. The caller owns the input buffers it passes in and all output buffers it supplies. Handle values are opaque identifiers: do not parse them, persist them as document IDs across kernel lifetimes, or use them with a different kernel.

## 2. Create a kernel

A kernel **requires** an allocator vtable. Start from a zero-initialized config, set `struct_size`, set `abi_major` to the expected ABI major, then fill in the allocator callbacks. The simple allocator below is suitable for a basic application:

```c
static void *app_alloc(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}
static void *app_realloc(void *ctx, void *p, size_t old_n, size_t new_n) {
    (void)ctx;
    (void)old_n;  /* libc realloc() does not use the old size */
    return realloc(p, new_n);
}
static void app_free(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)n;      /* the API supplies the exact size; free() ignores it */
    free(p);
}

cdin_kernel_config cfg = {0};
cfg.struct_size = (uint32_t)sizeof cfg;
cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
cfg.allocator.struct_size = (uint32_t)sizeof cfg.allocator;
cfg.allocator.alloc = app_alloc;
cfg.allocator.realloc = app_realloc;
cfg.allocator.free = app_free;
/* cfg.lock remains empty: use this kernel from one thread at a time. */

cdin_kernel *kernel = NULL;
cdin_status st = cdin_kernel_create(&cfg, &kernel);
if (st != CDIN_OK) {
    /* Handle the error; kernel is not ready for use. */
}
```

All fields not deliberately set should remain zero. A zero limit means “use the default,” not “allow nothing.” The allocator callback rules are strict: allocations request at least one byte; `realloc` receives the old and new allocation sizes; `free` receives the exact current size of the allocation. See [ABI reference](abi.md#memory-and-struct-rules).

To close the kernel, call `cdin_kernel_destroy(kernel)` only after stopping other threads that could call into it. Destroy also reclaims remaining documents, snapshots, transactions, and subscriptions. Prefer to release/close/unsubscribe explicitly first so ownership is clear; a live snapshot at destroy time is logged if a log sink is configured.

## 3. Create and close a document

Input is a byte pointer plus a length. It is **not** a C string: embedded `0x00` bytes are preserved, and no trailing NUL is added.

```c
const uint8_t initial[] = { 'a', 'b', 'c', '\n' };
cdin_doc doc = CDIN_INVALID_DOC;
cdin_status st = cdin_doc_create(kernel, initial, sizeof initial, &doc);
```

A new document starts at version `0`. An empty document is valid; pass `bytes = NULL` and `len = 0`. A nonzero length requires a non-NULL byte pointer.

`cdin_doc_close(kernel, doc)` closes and invalidates the document handle, aborts any active transaction for that document, and emits a final `CDIN_NOTIF_CLOSED` event to active subscribers. Calling it with a stale/closed handle returns `CDIN_INVALID_HANDLE`. Snapshots already acquired from the document remain readable until released or the kernel is destroyed.

## 4. Read the bytes

All read and edit coordinates are **byte offsets**. Ranges use the half-open form `[start, end)`: `start` is included and `end` is excluded.

```c
uint64_t length = 0;
cdin_status st = cdin_text_len(kernel, cdin_src_doc(doc), &length);
```

For a whole-document read, first query the required number of bytes, allocate that much memory, then read again:

```c
size_t needed = 0;
cdin_source source = cdin_src_doc(doc);
cdin_status st = cdin_text_read(kernel, source, 0, length,
                                NULL, 0, &needed, NULL);
/* Check st, allocate `needed` bytes, then call cdin_text_read again
   with the allocated buffer and capacity >= needed. */
```

`buf == NULL` is a size query and returns `CDIN_OK`; `*out_len` receives the required count. If a non-NULL buffer is too small, the call returns `CDIN_BUFFER_TOO_SMALL` and writes the required count to `*out_len`. Use `cdin_text_serialize` for a whole-document copy with the same size-query pattern. Never assume the byte sequence is NUL-terminated; use the returned length for output and string construction.

The `flags` output of `cdin_text_read` is optional. If supplied, `CDIN_READ_STARTS_MID_SEQUENCE` and `CDIN_READ_ENDS_MID_SEQUENCE` tell you when a range boundary falls inside a valid multibyte UTF-8 sequence. A byte read can deliberately split a sequence; the kernel reports it rather than repairing it.

## 5. Edit safely with versions

`cdin_doc_replace` replaces `[start, end)` with the exact bytes provided. Initialize request/result structures to zero, set their `struct_size`, and set the `expected_version` to the version on which your edit was based.

```c
const uint8_t insert[] = "XY";
cdin_replace_request req = {0};
req.struct_size = (uint32_t)sizeof req;
req.expected_version = 0;   /* the initial document version */
req.start = 1;
req.end = 2;                /* replace exactly one byte */
req.bytes = insert;
req.len = sizeof insert - 1;

cdin_edit_result result = {0};
result.struct_size = (uint32_t)sizeof result;
cdin_status st = cdin_doc_replace(kernel, doc, &req, &result);
if (st == CDIN_OK) {
    /* Track result.new_version and use it for the next version-checked edit. */
}
```

Rules to remember:

- New documents start at version `0`. A successful content-changing edit normally increments the version by one.
- A no-op edit leaves the version unchanged.
- If the document is no longer at `expected_version`, the mutation fails with `CDIN_STALE_VERSION`. Read the current version/content, recompute the edit, and retry deliberately; do not blindly replay old byte offsets.
- `CDIN_ANY_VERSION` disables the stale-version guard. Use it only when overwriting whichever version is current is truly safe.
- Failed mutations do not publish a partial edit. Treat other output values as unusable on non-`CDIN_OK`, except diagnostic fields explicitly documented for the result type.
- `result.start`, `removed`, and `inserted` describe the single primitive edit only when `edit_count == 1`. For a batch or a multi-edit history operation those fields are zero; use notifications for the full set of changes.

`cdin_doc_replace_batch` applies multiple ascending, non-overlapping ranges against one version as a single undo unit. Every range uses coordinates in the pre-edit document; all array arguments must contain at least `count` entries. The `coalesce_key` must be zero for a batch.

`cdin_doc_reload` replaces all content and resets history to the reloaded/saved state. The `CDIN_RELOAD_KEEP_HISTORY` flag is reserved but currently unsupported, so send `flags = 0`.

## 6. Undo, redo, saved state, and metadata

Each successful change is recorded as an undo unit unless no content change occurred. A new edit after undo discards the redo tail. Undo history is bounded by `max_undo_memory`; old units may be evicted to stay within the configured limit.

- `cdin_doc_undo` and `cdin_doc_redo` require a `cdin_history_request` with `struct_size` and an expected version. If there is no unit to undo/redo, the function returns `CDIN_INVALID_STATE`.
- `cdin_doc_mark_clean` records the current history position as the saved-state marker. `cdin_doc_is_clean` reports whether the document is at that marker. It does not write a file to disk.
- `cdin_doc_set_metadata` / `cdin_doc_get_metadata` store one caller-owned `uint64_t` of metadata per document. The kernel does not assign meaning to the value.
- `cdin_doc_history_info` reports available undo/redo units, evicted-unit count, and bytes used by history.
- Optional `blob_before` / `blob_after` byte strings on edits are opaque application state. On undo/redo, a caller can receive the relevant blob through `blob_out`; if `blob_capacity` is too small, the result's `details.required` gives the required size. These blobs are not parsed by the kernel.

The clean marker refers to history state, not a filename or a disk transaction. Applications should only call `cdin_doc_mark_clean` after their own save operation actually succeeds.

## 7. Transactions: group several edits atomically

Use a transaction when several edits must become visible as one commit/undo unit:

1. Call `cdin_txn_begin` with the version the transaction was based on.
2. Use `cdin_txn_replace` to edit the transaction's tentative text.
3. Use `cdin_txn_read` / `cdin_txn_len` to inspect that tentative version.
4. Call `cdin_txn_commit` to publish it, or `cdin_txn_abort` to discard it.

While a transaction is active, another mutation of the same document returns `CDIN_BUSY`. Ordinary document reads continue to see the pre-transaction contents; only transaction reads see tentative content. Nested transactions are rejected. Transaction replacement requests must use `expected_version = CDIN_ANY_VERSION`, `coalesce_key = 0`, and zero-length/no blobs; the commit request supplies optional before/after blobs. An unsuccessful commit rolls the transaction back and invalidates its transaction handle. An empty transaction can commit without changing the document version.

A transaction handle is temporary. Do not use it after successful commit, abort, shutdown, or a failed commit that has rolled it back.

## 8. Snapshots and read consistency

`cdin_snapshot_acquire(kernel, doc, &version, &snapshot)` captures a read-only view and the corresponding version. Build a source with `cdin_src_snap(snapshot)` and pass it to the same text read/position/search functions that accept a document source.

A snapshot is immutable: subsequent document changes do not alter its contents or version. This makes it useful for background search, rendering, diffing, or other reads that must agree on one version. Release it with `cdin_snapshot_release`. Snapshots count toward `max_snapshots_per_doc`, so release them promptly.

`cdin_source` must select exactly one source: either `{ .doc = doc, .snapshot = 0 }` or `{ .doc = 0, .snapshot = snapshot }`. The helpers `cdin_src_doc` and `cdin_src_snap` build these values safely. Setting both or neither yields `CDIN_INVALID_ARGUMENT`.

## 9. Text is bytes first; UTF-8 is a view

The kernel preserves all input bytes exactly. It does not:

- remove a UTF-8 BOM;
- replace invalid byte sequences with U+FFFD;
- normalize Unicode (for example NFC/NFD);
- treat `0x00` as an end-of-text marker;
- silently convert LF, CRLF, and CR line endings.

For position helpers, well-formed UTF-8 sequences form code points. Bytes that cannot form a valid sequence are treated as **raw bytes**, one byte per position unit. `cdin_text_region_at` tells you whether the region containing an offset is valid UTF-8 or raw data. Code-point conversions set `CDIN_CONV_TOUCHED_RAW` when the conversion crosses raw bytes.

### Byte offsets and valid positions

- A byte offset may be in `[0, document_length]`.
- An offset inside a valid multi-byte UTF-8 sequence is not a valid text position and normally returns `CDIN_INVALID_POSITION` in position-checked operations.
- An offset at the end of the document is a valid position.
- Inside a raw region, each byte is an independent unit.
- `cdin_text_check_position` also accepts `expected_version`; pass `CDIN_ANY_VERSION` only when a stale-position check is unnecessary.

### Code points, grapheme clusters, and columns

- `cdin_text_byte_to_codepoint` converts a valid byte boundary to a code-point index.
- `cdin_text_codepoint_to_byte` converts an index back to a byte offset.
- `cdin_text_codepoint_next` / `prev` move to the next/previous code-point-unit boundary.
- `cdin_text_grapheme_next` / `prev` move by extended grapheme-cluster boundaries using the Unicode tables pinned by the release (queried with `cdin_unicode_version`). Raw bytes each form their own cluster.
- `cdin_text_line_from_offset` reports a zero-based line and a **byte** column, not a grapheme or terminal-display column. Use grapheme stepping for cursor movement and a separate display-width implementation for terminal layout.

### Lines and line endings

The kernel recognizes LF (`0A`), CRLF (`0D 0A`, one terminator), and lone CR (`0D`). `cdin_text_line_count` is at least one: an empty document has one empty line, and text ending in a line terminator has a final empty line. `cdin_text_line_start` uses zero-based line numbers. `cdin_text_eol_info` counts LF, CRLF, and CR terminators and reports whether the final line lacks a terminator and whether styles are mixed.

## 10. Search

`cdin_search` has two supported modes:

- `CDIN_SEARCH_LITERAL` — the pattern is valid UTF-8 and matches only at valid text/code-point boundaries, skipping raw byte regions. It supports optional simple Unicode case folding (`CDIN_SEARCH_CASE_INSENSITIVE`) and optional newline equivalence (`CDIN_SEARCH_ANY_NEWLINE`, where LF in the pattern can match LF, CRLF, or CR).
- `CDIN_SEARCH_BYTES` — exact byte matching, including within raw regions. Case-fold and newline-equivalence flags are not allowed in this mode.

`CDIN_SEARCH_REGEX` is currently unsupported and returns `CDIN_UNSUPPORTED`.

Important request rules:

- A pattern must be nonempty. In literal mode it must be valid UTF-8.
- `ranges` stores pairs of `uint64_t`: `[start0, end0, start1, end1, ...]`. `ranges_capacity` is the number of **pairs**, not the number of integers. `limit` must be nonzero and no larger than this pair capacity.
- `from` is the first candidate start for forward search and the inclusive candidate start for backward search. `bound = UINT64_MAX` means the end of the source; in backward search it also bounds match ends.
- Matches do not overlap. For forward search, continue from `result.next_from`; for backward search use `result.next_from` and `result.next_bound` exactly as returned.
- A nonzero `step_limit` overrides the configured search step budget. A cancelled/over-budget search returns `CDIN_CANCELLED` / `CDIN_LIMIT_EXCEEDED`. To keep matches found so far, set `CDIN_SEARCH_WANT_PARTIAL`; then check `result.partial` and do not mistake the result for an exhaustive search.
- `result.skipped_raw` indicates the scanned literal-search window encountered raw data. `result.exhausted` indicates no further candidate remains. `result.truncated` means the match limit was reached.
- Results are tied to `result.version`. Use `expected_version` unless the source version is deliberately unconstrained.

Read the comments on `cdin_search_request` and `cdin_search_result` in the public header before building a pager around `next_from` / `next_bound`.

## 11. Notifications

Subscribe to changes with `cdin_notif_subscribe`. It returns a notification handle and the version at which the subscription starts. Mutations enqueue bounded events; `cdin_notif_drain` copies up to `max` events into a caller-supplied array. `cdin_notif_unsubscribe` discards pending events, invalidates the handle and frees the subscription state. A later drain/unsubscribe returns `CDIN_INVALID_HANDLE`. If the document closes, its final `CDIN_NOTIF_CLOSED` event can be drained before you unsubscribe.

Notification queues never block edits. If a queue overflows, it collapses to a `CDIN_NOTIF_RESYNC` event; treat this as “incremental event history is no longer complete” and re-read the latest document version rather than trying to replay missed edits. If `edits_truncated` is set, only the stored first entries are present; do not replay them as if they described the full change.

The optional wakeup callback runs on the thread that made the mutation, after the kernel lock is released. Its job is to **signal or enqueue work only** (for example, wake an event loop). Do not retain event pointers supplied by your own drain buffer; the kernel fills caller-owned event structs. The `CDIN_NOTIF_CLOSED` event is the final document event; after a document closes, the subscription no longer receives updates.

## 12. Threads and shutdown

Without a lock vtable, all calls into a kernel must be made from one thread at a time. If multiple threads must call into the same kernel, provide a matching `lock`/`unlock` pair that protects the same mutex and ensure `ctx` remains valid for the kernel's lifetime. The kernel invokes wakeup callbacks only after unlocking. `cdin_kernel_destroy` is special: stop all users first, and never call it concurrently with another API call.

`cdin_kernel_shutdown` stops new content mutations, aborts active transactions, and leaves existing documents available for reads and explicit close. Snapshot reads/releases and notification drains/unsubscribes remain usable for teardown. `CDIN_SHUTDOWN_GRACEFUL` and `CDIN_SHUTDOWN_ABORT` are reserved distinctions; in this implementation there are no accepted requests waiting in a queue, so the two modes behave identically. Destroy the kernel after clients have completed teardown.

## 13. Check status on every call

Every operation that returns `cdin_status` should be checked. For diagnostics use `cdin_status_name(status)`; for version-specific behavior inspect `cdin_abi_version` and `cdin_features`. Frequent recovery paths include:

| Status | Typical response |
|---|---|
| `CDIN_STALE_VERSION` | Re-read current version/content and recompute the edit. |
| `CDIN_INVALID_POSITION` | Recompute the byte offset at a UTF-8 boundary; do not round offsets silently. |
| `CDIN_BUFFER_TOO_SMALL` | Allocate at least the returned required byte count and retry. |
| `CDIN_BUSY` | Finish/abort the active transaction before trying another mutation. |
| `CDIN_LIMIT_EXCEEDED` | Inspect `details.limit_id`, increase the corresponding limit when creating the kernel, or reduce the operation. |
| `CDIN_INVALID_HANDLE` | Verify handle lifetime and that it belongs to this kernel. |
| `CDIN_SHUTDOWN` | Stop issuing new mutation calls and finish teardown. |
| `CDIN_UNSUPPORTED` | Remove reserved flags or use a supported feature path; regex search is unsupported. |

For field-level diagnostics and all enum values see [limits and status](limits-and-status.md) and [ABI reference](abi.md).
