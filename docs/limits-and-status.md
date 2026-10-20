# Limits, status codes, and feature support

This page distinguishes an available API from a feature that is only reserved in the header. For exact prototypes, see the [ABI reference](abi.md); for safe call patterns, see the [user guide](user-guide.md).

## Default limits

Each zero-valued limit in `cdin_kernel_config` selects the default shown here.

| Limit | Default | Notes |
|---|---:|---|
| `max_documents` | 1,024 | Maximum live documents owned by one kernel. Closed documents no longer count as live. |
| `max_document_size` | 1 GiB | Bytes per document; also capped relative to `SIZE_MAX`. |
| `max_edit_size` | `max_document_size` | Maximum bytes inserted by one edit; clamped not to exceed the document cap. |
| `max_undo_memory` | 64 MiB | History memory per document. Older units may be evicted at the cap. |
| `max_blob_size` | 4,096 bytes | Maximum size of each application-state blob before/after an undo unit. |
| `max_snapshots_per_doc` | 16 | Live snapshots attached to a document. |
| `max_subscribers_per_doc` | 16 | Per-document subscriptions; hard implementation cap is 64. |
| `notif_queue_capacity` | 64 events | Bounded ring per subscriber, clamped to 4–65,536. |
| `default_search_steps` | 4,294,967,296 | Candidate start positions; used when a search's `step_limit` is zero. |
| `CDIN_MAX_BATCH_EDITS_HARD` | 65,536 | Absolute batch edit count ceiling; exceeding it reports `CDIN_LIMIT_BATCH_EDITS`. |

`CDIN_LIMIT_TXN_MEMORY` is a limit identifier, not a separate configuration field. Active transaction logging and retained history share the `max_undo_memory` per-document budget.

If an operation returns `CDIN_LIMIT_EXCEEDED`, inspect the returned `details.limit_id` when the result type exposes it. Reduce the operation or set the relevant config field to an appropriate nonzero value. A value of zero selects the default; it is not a way to disable a resource.

## Current support matrix

| Capability | Status in this codebase | Notes |
|---|---|---|
| Create/close documents | Supported | New documents start at version 0. Content is arbitrary bytes. |
| Byte-range replace | Supported | Version-checked; range is `[start, end)`. |
| Multi-range/batch replace | Supported | Ascending, non-overlapping pre-edit ranges; one undo unit. |
| Undo / redo | Supported | Bounded history; a new edit after undo discards redo tail. |
| Coalesced edits | Supported | Ordinary replace can use a nonzero `coalesce_key`; batches cannot. |
| Opaque app-state blobs | Supported | Stored in undo units and returned by undo/redo when requested. |
| Transactions | Supported | Tentative text is visible only through the transaction handle; a failed commit rolls back. |
| Immutable snapshots | Supported | Content stays pinned to the acquired version until released. |
| Text read/serialize | Supported | Exact bytes, size query, and too-small-buffer reporting. |
| UTF-8/raw-byte classification | Supported | Invalid UTF-8 is preserved and surfaced as raw bytes. |
| Code-point and grapheme stepping | Supported | Unicode data version is queried by `cdin_unicode_version`; raw bytes are independent units. |
| LF / CRLF / CR line handling | Supported | Empty documents have one line; a final terminator creates a final empty line. |
| Literal search | Supported | Requires a valid UTF-8 pattern; skips raw data for matching. |
| Byte search | Supported | Exact byte matching can include raw data. |
| Simple case-insensitive literal search | Supported | Uses `CDIN_SEARCH_CASE_INSENSITIVE`; do not confuse it with locale-sensitive collation. |
| Any-newline literal search | Supported | LF in the pattern may match LF, CRLF, or CR. |
| Regex search | **Not supported** | `CDIN_SEARCH_REGEX` returns `CDIN_UNSUPPORTED`; feature bit is not advertised. |
| Reload while keeping history | **Not supported** | `CDIN_RELOAD_KEEP_HISTORY` returns `CDIN_UNSUPPORTED`; pass flags `0`. |
| Lock-vtable feature reporting | Supported | `CDIN_FEAT_LOCKING` is advertised; a configured lock/unlock pair serializes API calls. Without it, callers must serialize calls themselves. |
| Graceful vs abort shutdown | Same behavior today | No accepted mutation requests wait in an internal queue, so the modes behave identically. |

## Status-to-action table

| Status | Action |
|---|---|
| `CDIN_INVALID_ARGUMENT` | Check NULL pointers, lengths, ranges, required `struct_size`, reserved flags, and mutually exclusive fields. |
| `CDIN_INVALID_HANDLE` | Check lifetime and handle type; do not reuse a released snapshot or closed document. |
| `CDIN_STALE_VERSION` | Re-read and recompute the requested operation against the current version. |
| `CDIN_OUT_OF_RANGE` | Check offsets against the current byte length and indices against the corresponding count. |
| `CDIN_INVALID_POSITION` | Move to a UTF-8 sequence boundary; the API does not silently round invalid positions. |
| `CDIN_INVALID_STATE` | Check whether history/transaction/document state permits that operation. |
| `CDIN_BUSY` | Finish or abort the active transaction before mutating the same document again. |
| `CDIN_LIMIT_EXCEEDED` | Check `details.limit_id`, limits, match budget, and buffer/resource sizes. |
| `CDIN_NO_MEMORY` | Keep the previous state; free caller-owned temporary allocations and optionally retry. |
| `CDIN_PATTERN_INVALID` | Use a valid UTF-8 pattern for literal search, or select byte mode when exact raw-byte matching is intended. |
| `CDIN_CANCELLED` | Stop or continue a search only if partial-result semantics were explicitly requested. |
| `CDIN_SHUTDOWN` | Stop mutations and finish teardown. |
| `CDIN_UNSUPPORTED` | Remove unsupported flags or use a supported feature. |
| `CDIN_BUFFER_TOO_SMALL` | Resize to the returned required length and retry. |
| `CDIN_VERSION_MISMATCH` | Build/configure the binding against a compatible ABI major. |
| `CDIN_INTERNAL` | Capture logs/reproduction data and report a kernel defect. |

## Important edge cases

- Input strings may include embedded NULs or invalid UTF-8. Always pass an explicit length.
- A document read can begin/end within a multibyte sequence; check read flags if that matters to your UI. Position-sensitive mutations must use valid positions.
- Search `ranges_capacity` is a count of **matches/pairs**, not a count of `uint64_t` cells. Allocate at least `2 * ranges_capacity` cells.
- Notification overflow is represented by `CDIN_NOTIF_RESYNC`. Incremental edit replay is no longer sufficient after that event.
- `cdin_notif_unsubscribe` invalidates and frees the subscription handle. A later drain/unsubscribe returns `CDIN_INVALID_HANDLE`; a closed document may emit one final `CDIN_NOTIF_CLOSED` event that remains drainable until the caller unsubscribes.
- If snapshots are not released, they count against the configured snapshot cap and can retain old text blocks in memory.


ABI 1.0.0 also guards version and accepted-sequence exhaustion: the kernel does not wrap a document version into `CDIN_ANY_VERSION` or wrap its sequence counter. It returns `CDIN_LIMIT_EXCEEDED` with `CDIN_LIMIT_VERSION` or `CDIN_LIMIT_SEQUENCE`. The batch ceiling is reported with `CDIN_LIMIT_BATCH_EDITS`.
