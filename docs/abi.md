# C ABI reference

The public C ABI is declared in [`include/cdin/kernel.h`](../include/cdin/kernel.h), the only public header. This page explains the rules shared by functions and gives a directory of the exported surface; the header is the final authority for exact field order, C types, constants, and function prototypes.

**Reported ABI version:** `0.2.0` (`CDIN_KERNEL_ABI_MAJOR/MINOR/PATCH`). The project is still marked draft. Bindings should compile against the exact header that ships with the library and query the loaded library version at startup.

## 1. ABI fundamentals

The API is an **in-process native C ABI**. Calls execute synchronously in the caller process; this repository does not define an IPC, socket, RPC, or wire protocol. The optional wakeup callback is a notification hook, not an asynchronous request queue.

### Status-returning functions

Most public functions return `cdin_status`; data is usually delivered through out-parameters. `cdin_status_name()` is the exception: it returns a static NUL-terminated status-name string. Check every status instead of assuming a function succeeded.

Common patterns:

- A non-NULL output pointer is generally required for every requested result.
- `cdin_edit_result` passed to edit/history operations must be zero-initialized and have `struct_size = sizeof(cdin_edit_result)`.
- Input request structs must be zero-initialized, then have `struct_size = sizeof(request)` and only supported fields set.
- Unknown flags are not ignored; reserved nonzero flags commonly return `CDIN_UNSUPPORTED` or `CDIN_INVALID_ARGUMENT`.
- On failure, do not consume ordinary result fields. The error-details fields documented for the specific result type may be filled on failure.
- Byte data is always paired with a length. Do not use NUL-terminated-string assumptions for document content.

### ABI and feature queries

- `cdin_abi_version(&major, &minor, &patch)` returns the ABI version.
- `cdin_features(&mask)` returns the compile-time feature mask.
- `cdin_unicode_version(&major, &minor, &patch)` returns the Unicode data version.

The current feature mask advertises batch edits, transactions, snapshots, notifications, literal search, byte search, grapheme stepping, and simple case folding. Regex is not advertised. `CDIN_FEAT_LOCKING` is defined in the header but is not included in the current feature-mask result, even if a caller configures a lock vtable; do not use that bit as a runtime lock probe.

## 2. Status codes

Treat the symbolic enum names as the contract. Do not persist numeric status values in files or protocols.

| Status | Meaning and typical cause |
|---|---|
| `CDIN_OK` | Operation succeeded. |
| `CDIN_INVALID_ARGUMENT` | NULL/contradictory arguments, bad ranges, malformed request struct, or invalid parameter combination. |
| `CDIN_INVALID_HANDLE` | Handle is stale, invalid, released, closed, or of the wrong handle family. |
| `CDIN_STALE_VERSION` | `expected_version` does not match the current source/document version. |
| `CDIN_OUT_OF_RANGE` | Byte offset/range or requested index is outside the valid extent. |
| `CDIN_INVALID_POSITION` | A position-sensitive operation was given an offset inside a valid multibyte UTF-8 sequence. |
| `CDIN_INVALID_STATE` | Operation is not valid in the current state, such as undo with no undo unit or nested transaction. |
| `CDIN_BUSY` | Operation conflicts with an active transaction on the document. |
| `CDIN_LIMIT_EXCEEDED` | Configured/default limit or search-step budget was exceeded. |
| `CDIN_NO_MEMORY` | The supplied allocator failed an allocation. |
| `CDIN_PATTERN_INVALID` | Literal-search pattern is invalid UTF-8. |
| `CDIN_CANCELLED` | Search cancellation flag was observed. |
| `CDIN_SHUTDOWN` | Kernel is shut down and this operation is disallowed. |
| `CDIN_UNSUPPORTED` | Feature, flag, or forward extension is not supported by this build. |
| `CDIN_BUFFER_TOO_SMALL` | Caller buffer is smaller than the required output. |
| `CDIN_VERSION_MISMATCH` | Caller requested a different ABI major in `cdin_kernel_config.abi_major`. |
| `CDIN_INTERNAL` | Internal consistency failure; preserve diagnostics and treat as a bug. |

Use `cdin_status_name(status)` in logs. For `CDIN_LIMIT_EXCEEDED`, inspect `details.limit_id` where the operation exposes details. For `CDIN_STALE_VERSION`, inspect the `expected` and `current` detail fields where provided. For `CDIN_BUFFER_TOO_SMALL`, `required` gives the needed byte count on edit/history results; text reads expose the needed count through `out_len`.

## 3. Handles, sources, and versions

| Type | Role |
|---|---|
| `cdin_kernel *` | Opaque kernel pointer, created/destroyed by the library. |
| `cdin_doc` | 64-bit document handle; zero is invalid. |
| `cdin_txn` | 64-bit transaction handle; zero is invalid. |
| `cdin_snapshot` | 64-bit snapshot handle; zero is invalid. |
| `cdin_notif` | 64-bit notification subscription handle; zero is invalid. |
| `cdin_version` | 64-bit document/source version; fresh documents start at zero. |
| `cdin_source` | Selects exactly one of `doc` and `snapshot`. |

`CDIN_ANY_VERSION` is `UINT64_MAX` and disables a version comparison. Use it only when it is safe to operate on whichever version is current. Handles are kernel-scoped and opaque; do not decode their bits.

`cdin_src_doc(d)` and `cdin_src_snap(s)` are inline helpers for constructing `cdin_source` correctly. For every source, exactly one field must be nonzero.

## 4. Memory and struct rules

### Allocator vtable

`cdin_kernel_create` requires all of `cdin_allocator.alloc`, `realloc`, and `free`:

- `alloc(ctx, n)`: `n >= 1`; returns NULL on failure.
- `realloc(ctx, p, old_n, new_n)`: `p` is non-NULL and both sizes are at least one; it must preserve the first `min(old_n, new_n)` bytes on success. `old_n` must be the exact current allocation size.
- `free(ctx, p, n)`: `n` is the exact current allocation size.

The callbacks and `ctx` must remain valid until `cdin_kernel_destroy` finishes. All core-owned allocations use this allocator. A simple libc allocator can ignore `old_n`/`n` internally, but a diagnostic/custom allocator should verify them.

### `struct_size`

Every request/configuration struct passed into the kernel starts with `struct_size`. Set it to `sizeof(the_struct)` for the header being compiled against. The code accepts a declared struct size at least as large as the known struct only when any unknown trailing bytes are zero. Do not fabricate a larger size without allocating and zeroing that entire declared range.

The common request fields `flags` are reserved unless the header explicitly documents otherwise. Keep them zero for forward compatibility.

### Buffer sizing

`cdin_text_read` and `cdin_text_serialize` support the two-call pattern: first pass `buf = NULL` to learn the needed byte count; then allocate and call again. If a supplied buffer is too small, `CDIN_BUFFER_TOO_SMALL` reports the required length. The caller owns all input and output buffers; no document buffer pointer is borrowed and retained by the core.

### `cdin_edit_result` and `cdin_error_details`

`cdin_edit_result` contains the new version, accepted mutation sequence number, new length, count of primitive edits, single-edit coordinates when meaningful, undo/redo blob length, and an embedded `cdin_error_details`. `details` may be set on an error; do not assume other result fields are valid unless the call returned `CDIN_OK`.

`cdin_error_details` includes:

- `limit_id`: the exceeded `CDIN_LIMIT_*` category;
- `expected`, `current`: version mismatch details;
- `requested`, `length`: offending offset and available length for range/position errors;
- `required`: output/blob buffer size when reported.

## 5. Configuration and defaults

`cdin_kernel_config` supplies the ABI major, allocator, optional lock/log services, and per-kernel limits. A zero limit selects the corresponding default:

| Configuration field / macro | Default | Scope / units |
|---|---:|---|
| `max_documents` | `1024` | Live documents per kernel. |
| `max_document_size` | `1 GiB` | Maximum document bytes. Implementation also caps it to a safe fraction of `SIZE_MAX`. |
| `max_edit_size` | `max_document_size` | Maximum bytes inserted by one edit; clamped not to exceed max document size. |
| `max_undo_memory` | `64 MiB` | Undo-history bytes, per document. |
| `max_blob_size` | `4096` bytes | Maximum `blob_before`/`blob_after` payload per side. |
| `max_snapshots_per_doc` | `16` | Live snapshots per document. |
| `max_subscribers_per_doc` | `16` | Subscribers per document; implementation hard cap is `64`. |
| `notif_queue_capacity` | `64` events | Per subscriber; implementation clamps it to `4..65536`. |
| `default_search_steps` | `2^32` | Candidate starts examined for a search whose `step_limit` is zero. |

`CDIN_LIMIT_TXN_MEMORY` is reserved as a reported limit identifier, but the current `cdin_kernel_config` has no dedicated transaction-memory limit field. Transactions are still bounded indirectly by document/edit limits and available memory.

## 6. Thread-safety contract

If `cdin_kernel_config.lock.lock` and `.unlock` are both NULL, all calls on one kernel must come from one thread at a time. If one is set, both must be set; they must lock/unlock the same application-owned synchronization object passed as `ctx`. Keep that object alive for the full kernel lifetime.

With a lock vtable, ordinary API calls are serialized by that caller-supplied lock. Search pins a ref-counted immutable text block, releases the lock while scanning, then reacquires it to release the block; it does not scan a changing buffer. Wakeup callbacks are called only after the kernel lock is released and should signal/enqueue work rather than do lengthy processing. `cdin_kernel_destroy` is always owner-thread/exclusive: stop all other calls before destroying.

## 7. Function directory

The following list is grouped by responsibility. Click the corresponding declaration in the public header for the exact prototype and field names.

### Status, versions, and lifecycle

- `cdin_status_name`
- `cdin_abi_version`, `cdin_features`, `cdin_unicode_version`
- `cdin_kernel_create`, `cdin_kernel_destroy`, `cdin_kernel_shutdown`, `cdin_kernel_last_sequence`

### Documents, edit history, and metadata

- `cdin_doc_create`, `cdin_doc_close`, `cdin_doc_reload`
- `cdin_doc_replace`, `cdin_doc_replace_batch`
- `cdin_doc_undo`, `cdin_doc_redo`
- `cdin_doc_mark_clean`, `cdin_doc_is_clean`
- `cdin_doc_set_metadata`, `cdin_doc_get_metadata`, `cdin_doc_history_info`

### Transactions

- `cdin_txn_begin`, `cdin_txn_replace`, `cdin_txn_commit`, `cdin_txn_abort`
- `cdin_txn_len`, `cdin_txn_read`

### Snapshots and source reads

- `cdin_snapshot_acquire`, `cdin_snapshot_release`
- `cdin_text_version`, `cdin_text_len`, `cdin_text_read`, `cdin_text_serialize`

### Text positions and line endings

- `cdin_text_check_position`, `cdin_text_region_at`
- `cdin_text_line_count`, `cdin_text_line_start`, `cdin_text_line_from_offset`, `cdin_text_eol_info`
- `cdin_text_byte_to_codepoint`, `cdin_text_codepoint_to_byte`
- `cdin_text_codepoint_next`, `cdin_text_codepoint_prev`
- `cdin_text_grapheme_next`, `cdin_text_grapheme_prev`

### Notifications

- `cdin_notif_subscribe`, `cdin_notif_drain`, `cdin_notif_unsubscribe`

### Search

- `cdin_search`

## 8. Edit and search request structs

| Struct | Primary fields and constraints |
|---|---|
| `cdin_reload_request` | `expected_version`, byte pointer/length; `flags` must be zero; `CDIN_RELOAD_KEEP_HISTORY` is unsupported. |
| `cdin_replace_request` | Version, half-open byte range, replacement pointer/length, optional coalesce key, optional opaque before/after blobs. |
| `cdin_batch_request` | One expected version; ascending non-overlapping starts/ends; pointer/length arrays of `count` elements; zero coalesce key. |
| `cdin_history_request` | Expected version and optional caller buffer for the undo/redo state blob. |
| `cdin_txn_begin_request` | Expected base version; flags zero. |
| `cdin_txn_commit_request` | Optional before/after blobs committed as part of the one undo unit. |
| `cdin_subscribe_request` | Optional wakeup callback and context; flags zero. |
| `cdin_search_request` | Source, expected version, pattern and length, mode/direction, from/bound, match limit, step limit, optional cancel pointer, and result pair buffer. |

For transaction edits, set `expected_version = CDIN_ANY_VERSION`, `coalesce_key = 0`, and zero blob lengths; all positions are validated against the transaction's tentative content. For ordinary edits, the expected version should normally equal the version read immediately before computing the edit.

For search, `ranges_capacity` counts **pairs**. A match occupies two `uint64_t` values (`start`, `end`). `limit` must be nonzero and no greater than `ranges_capacity`. Matches are non-overlapping. Regex returns `CDIN_UNSUPPORTED`.

## 9. Feature flags

Use symbolic flags, not numeric literals:

- `CDIN_FEAT_BATCH`, `CDIN_FEAT_TRANSACTIONS`, `CDIN_FEAT_SNAPSHOTS`, `CDIN_FEAT_NOTIFICATIONS`
- `CDIN_FEAT_SEARCH_LITERAL`, `CDIN_FEAT_SEARCH_BYTES`, `CDIN_FEAT_SEARCH_REGEX` (not set in current build)
- `CDIN_FEAT_GRAPHEME`, `CDIN_FEAT_CASEFOLD_SIMPLE`, `CDIN_FEAT_LOCKING` (defined but not reflected in the current feature mask)

Search flags are a separate enum-like bit field: `CDIN_SEARCH_CASE_INSENSITIVE`, `CDIN_SEARCH_ANY_NEWLINE`, and `CDIN_SEARCH_WANT_PARTIAL`. They are valid only as specified for the selected search mode.

## 10. Release and compatibility guidance

ABI version `0.x` and project status `draft` mean the layout has not yet been declared stable for independent binary releases. To ship an application:

1. Vendor or package the matching header and native library together.
2. Compile your FFI declarations from the header if possible; avoid hand-copying struct layouts across versions.
3. Query `cdin_abi_version` at startup and reject a library whose ABI major is incompatible.
4. Query `cdin_features` before enabling optional capabilities.
5. Rebuild the optional `examples/ffi` bridge and all language bindings whenever the header/core changes.

Do not expose kernel handles, raw pointers, or ABI structs over a network protocol or persist them as portable data. Convert them to your application's own stable types at the language boundary.
