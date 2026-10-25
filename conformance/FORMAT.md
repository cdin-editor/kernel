# Conformance Vector Format

Conformance vectors are executable examples of public behavior. Run the full gate with `make check`; a vector is not documentation-only prose, and an expected result must match the implementation's public contract.

Vectors live in `conformance/vec/<family>/`. The runner is in `conformance/runner/` and is built by `make check`.

## Vector header

    vector <name> covers <REQ-ID> [REQ-ID ...]

A new header starts a fresh vector: every live kernel is destroyed, allocator failure points are cleared, and handle names restart at `k1` / `ks1`.

## Steps

Each step is one line, `<op> [key=value ...]`. Values may be quoted (`bytes="a\x0Ab"`); inside quotes, `\xHH` is one byte and `\\` is one backslash. There are no other escapes: `\n` inside quotes is a backslash and an `n`.

A step may name a handle that was never issued (`handle=k99`); that is a runtime error (`CDIN_INVALID_HANDLE`), not a syntax error.

## Expectations

    expect <status> [key=value ...]

The status is a bare symbolic status name. Every listed key is checked; an unknown key is a syntax error, so an expectation can never pass silently.

| Key | Meaning |
|---|---|
| `major`, `minor`, `patch` | ABI or Unicode version-query result, depending on the preceding operation |
| `mask` | Result of `cdin_features` |
| `version` | Document version, search version, snapshot version, or the version returned by subscribe, depending on preceding operation |
| `length` | Document length, or length of newly created bytes |
| `offset` | Line start, byte offset for a code-point index, or metadata value |
| `codepoint`, `line`, `col` | Text-position outputs |
| `new_version`, `start`, `removed`, `inserted`, `new_length` | `cdin_edit_result` fields |
| `count` | Search match count, drained event count, or line count |
| `current_version`, `from_version`, `to_version`, `kind` | Fields in the first drained notification event |
| `next`, `prev` | Grapheme-boundary result |
| `clean`, `valid` | Saved-state marker or position-check result |
| `style` | End-of-line style (`none`, `lf`, `crlf`, `cr`, `mixed`) |
| `seq` | Last accepted mutation sequence, where applicable |
| `data` | Bytes returned by read/serialize/snapshot-read |
| `matches` | Search ranges in `start:end start:end ...` form |
| `handle` | Name of the handle created by the preceding operation |

## Handles and validation

`create`, `create_doc`, `txn_begin`, `subscribe`, and `snapshot_acquire` register returned handles under the next generated name: `k1`, `k2`, ... for kernels, documents, and transactions; `ks1`, `ks2`, ... for notification and snapshot handles. Names restart at `k1` in each vector. The handle table is typed: passing a notification handle where a document is required is a syntax error. If `create_doc` is used before an explicit `create`, the runner creates an implicit kernel that is not nameable.

Required keys are listed without brackets in the vector grammar. An unknown or duplicated key, or a missing required key, is a syntax error.

## Operations

| Op | Args | Description |
|---|---|---|
| `create` | `allocator=<name> [max_documents] [max_document_size] [max_undo] [max_blob]` | Create a kernel |
| `destroy` | `handle=<name>` | Destroy a kernel |
| `fail_alloc_after` | `allocator=<name> n=<number>` | Fail the named allocator's Nth allocation |
| `abi_version` | - | Query ABI version |
| `features` | - | Query advertised feature mask |
| `unicode_version` | - | Query embedded Unicode-data version |
| `create_doc` | `[bytes="<bytes>"]` | Create a document (empty by default) |
| `close_doc` | `handle=<name>` | Close a document |
| `version` / `len` | `handle=<name>` | Query version / byte length |
| `read` | `handle start end` | Read an explicit byte range |
| `serialize` | `handle=<name>` | Serialize the entire document |
| `replace` | `handle start end bytes expected_version [coalesce_key]` | Replace a byte range |
| `undo` / `redo` | `handle expected_version` | Undo or redo one unit |
| `txn_begin` | `handle [expected_version]` | Begin transaction |
| `replace_tx` | `handle start end bytes` | Replace inside a transaction |
| `txn_commit` / `txn_abort` | `handle` | Commit or abort transaction |
| `mark_clean` / `is_clean` | `handle` | Saved-state marker |
| `set_metadata` / `get_metadata` | `handle [value]` | Read/write caller metadata |
| `subscribe` / `unsubscribe` | `handle` | Subscribe/unsubscribe to notifications |
| `drain` | `handle` | Drain queued events |
| `snapshot_acquire` / `snapshot_release` | `handle` | Snapshot lifecycle |
| `snapshot_read` | `handle start end` | Read a snapshot range |
| `shutdown` | `mode=graceful\|abort` | Shut down the kernel |
| `search` | `handle pattern [mode] [direction] [from] [case_sensitive] [limit]` | Search document content |
| `line_count`, `line_start`, `line_from_offset`, `byte_to_line`, `line_to_byte` | see runner grammar | Line index operations |
| `byte_to_codepoint`, `codepoint_to_byte`, `check_position` | see runner grammar | UTF-8 position operations |
| `eol_style` | `handle` | Report line terminator style |
| `grapheme_next` / `grapheme_prev` | `handle offset` | Grapheme boundaries |

## Allocator checks

The `counting` allocator tracks allocation calls and can fail a chosen allocation. Each block carries a header, so `free` and `realloc` are checked against sizes reported by the kernel. A size mismatch or live allocation after kernel teardown is a test failure. These checks are part of the validation gate, not expected failures.

## Example

```text
vector abi.basic covers ABI-001 ABI-006 ABI-014 ABI-017 KERNEL-008 ERR-001 ERR-003
  create allocator=counting
  expect CDIN_OK
  abi_version
  expect CDIN_OK major=1 minor=0 patch=0
  features
  expect CDIN_OK
  destroy handle=k1
  expect CDIN_OK
```

## Exit codes

`build/conformance-runner <file>...` returns zero only when all vectors in the requested files pass. `make check-vectors` is the failing gate; `make vectors` is for reports and may continue after an individual failure. `make check-tests` runs the C regression tests.
