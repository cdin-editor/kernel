# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-10-20

### Added
- Initial public release of the `cdin-kernel` native C11 document-editing kernel.
- Public C API for kernel lifecycle, document handles, byte-preserving document
  creation and access, replacement edits, version checks, and serialization.
- Text utilities for UTF-8 region classification, line indexing, byte/code-point
  position conversion, and LF/CRLF/CR line-ending recognition.
- Undo/redo history, edit coalescing, transactions, clean-state tracking, and
  per-subscriber change notifications.
- Immutable snapshots, concurrency controls, and graceful/abort shutdown modes.
- Literal and byte-oriented search, including forward/backward and find-all
  operations; unsupported regex requests return `CDIN_UNSUPPORTED`.
- Conformance-vector runner, strict test allocator, regression/concurrency tests,
  and build targets for sanitizer, benchmark, fuzz, and trace workflows.
- Portable Makefile build and initial install/package support for native library
  consumers.

### Fixed
- Memory-lifetime, allocation-failure atomicity, document-table growth, edit-tail
  relocation, undo-history accounting, and serialization edge cases covered by
  the regression suite.
- CRLF line counting and line-index invalidation after edits.
- Conformance runner parsing and isolation so vector failures produce a non-zero
  exit status and process-scoped state does not leak between vector files.

### Compatibility and limitations
- This is an initial development release. The public API/ABI is not yet declared
  stable; consumers should pin the exact release and expect possible breaking
  changes before a stable 1.0.0 release.
- Batch replacement and regex search are not implemented and return
  `CDIN_UNSUPPORTED`.
- Regex engine integration, complete Unicode case folding, and full Unicode
  grapheme-cluster segmentation are not included in this release.
- The spec and implementation still require reconciliation before claiming full
  spec conformance. See `RELEASE-READINESS-REVIEW.md` when available in the
  release-work bundle for the current readiness assessment.

## [Unreleased]

### Changed - P7: layered source tree, full gate green

Gate green: 105 vectors (18 files), P1 (10 tests) + P3 (9 tests), ASan/UBSan
and TSan all report nothing on gcc 13.3.0 (Ubuntu 24.04 WSL2), bench and
4400-input fuzz smoke clean on MinGW 16.1.0.

- `src/` reorganised into `util/ core/ text/ search/ history/ doc/ txn/
  notif/ api/` plus `kernel.c`, one consolidated `src/internal.h`. Same
  behaviour, no ABI change (header draft 0.2.0 untouched).
- CONC semantics sharpened to the fixtures: sequence numbers only for content
  mutations; shutdown = stopped marker + abort of open transactions, documents
  stay readable and closeable; stale requests take no sequence number and
  write only `details`.
- Notified states: `SAVED_STATE_CHANGED` only on the dirty->clean transitions
  (mark_clean, reload); unsubscribe detaches and frees the ring while the
  handle stays drainable (NOTIF-010).
- Search failures publish only `details` unless `CDIN_SEARCH_WANT_PARTIAL`;
  fixtures updated to draft 0.2.0 semantics (`search.unicode.vec`).
- `support/strict_alloc.c` (test-support allocator) fixed: NULL vtable ctx on
  first allocation, realloc unlink/leak-count corruption; struct is now
  defined in the header (bench and fuzz instantiate it on the stack).
- Runner/test ports to the request-struct API; runner updated for the frozen
  search request/result shape (`cdin_search`, caller-owned range arrays).

### Added - P4: request struct and blob copy-out
Gate green: 100 vectors, P1 (10 tests) and P3 (9 tests) all pass; ASan, UBSan
and TSan report nothing on gcc 13.3.0 (Ubuntu 24.04 WSL2); 0 strict-allocator
violations.

- `cdin_replace_request`: replace parameters are now a struct. The kernel
  copies `req->bytes` at call time; the caller may free the buffer as soon as
  the call returns (ABI-008, ABI-018).
- `cdin_doc_get_last_blob`: two-call copy-out accessor for the caller blob
  recorded for the last committed unit. `which`: 0 = before (restored on undo),
  1 = after (restored on redo). `buf == NULL` reports the required size and
  returns OK (UND-020/021, ABI-018).
- `tests/test_p3_conc.c`: 9 concurrency tests with real OS threads (Win32 +
  POSIX) covering sequence density, version races, snapshot immutability,
  subscriber lifecycle, close/read races, shutdown during edits, two-kernel
  isolation, and allocation failure under load.

### Changed — P4: internal
- `conformance/runner/alloc_strict.c`: spinlock added for thread-safety
  (snapshot release frees blocks outside the kernel lock).
- `tests/test_p1.c`: updated for new `cdin_notif_subscribe` signature and
  `cdin_doc_replace` request struct; added `test_get_last_blob`.

### Added — P2: text semantics
Gate green: `text.eol.*`, `text.bytes.*` and `pos.*` pass; ASan, UBSan and TSan
report nothing on gcc 13.3.0 and clang 18.1.3 (Ubuntu 24.04); 0 strict-allocator
violations; 24 new vectors. Logs in `docs/baseline/p2/`.

- CRLF is one line terminator. The line index is cached per document and tagged
  with the version it describes, so an edit that joins or splits a CRLF pair
  invalidates it (TEXT-020, TEXT-021).
- `cdin_doc_eol_style`: none, lf, crlf, cr or mixed, without changing a byte
  (TEXT-022).
- `src/regions.c`: UTF-8 region classification by the Unicode maximal-subpart
  rule, specified in `docs/regions.md` and pinned by `text.regions.*` for
  `E2 80 41`, a lone `80`, `C0 80`, `ED A0 80`, `F4 90 80 80`, a sequence
  truncated at end of document, and a valid two-byte character for contrast
  (TEXT-010 to TEXT-012, TEXT-013).
- `cdin_doc_check_position` returns its verdict as a status: OK, OUT_OF_RANGE or
  INVALID_POSITION; the out flag mirrors it on success and is left untouched on
  failure like every other out-parameter (POS-020, POS-021).
- `cdin_doc_byte_to_codepoint` fails with INVALID_POSITION inside a multibyte
  sequence instead of rounding down, and counts each raw byte as one unit
  (POS-020, POS-031).

### Fixed — P2
- `cdin_doc_line_count` counted both bytes of a CRLF pair as two terminators.
- The line index was recomputed from scratch on every query and was not
  invalidated after an edit.
- `cdin_doc_serialize` (P1) made the two-call size report work; this phase adds
  the region and position work it depended on.
- `conc.shutdown_graceful` used a positional argument that the current runner
  rejects, so it had been failing as a syntax error and the kernel's
  process-wide shutdown flag was never exercised. Corrected, and the defect it
  uncovered is now pinned by a failing vector of its own
  (`conc.shutdown_is_per_kernel`, P3).
- `make check-vectors` now runs one process per vector file, so a defect with
  process scope cannot contaminate the results of unrelated files.

### Fixed — P1: memory safety and object lifetime
P1 gate green on Ubuntu 24.04 (gcc 13.3.0 and clang 18.1.3): ASan, UBSan and
TSan report nothing, LeakSanitizer reports no leaks, the strict allocator
reports 0 violations, and the failure-atomicity sweeps pass. Windows MinGW
builds the same sources warning-free but cannot link the sanitizer runtimes.
`make check-vectors` is 76 vectors: 62 pass, 14 fail (P2-P6 backlog).
Logs in `docs/baseline/p1/`; the P0 baseline in `docs/baseline/p0/` is untouched.

- `cdin_doc_create` now zeroes the whole document structure and zeroes the new
  region of the document and generation tables on growth. An uninitialised
  `tx_active` made the first edit answer `BUSY`; an uninitialised `undo` array
  made `undo` dereference a wild pointer and crash the run.
- Growth of the document table is failure-atomic: if the generation table cannot
  grow, the already-moved document table is handed back instead of dropped.
- `cdin_doc_replace` and `cdin_txn_replace` no longer move the tail from the
  buffer that `realloc` may already have released. Growing resizes first;
  shrinking moves the tail inside the block that still holds it and shrinks
  afterwards. A failing shrinking realloc is ignored, and the document keeps its
  logical length.
- Every allocation goes through `mem_alloc` / `mem_realloc` / `mem_free`, and
  every document records the true size of its block (`alloc_len`), so `free`
  and `realloc` always receive the size that was requested. An empty document
  owns no block, replacing the previous 1-byte floor.
- `cdin_kernel_destroy` frees document bytes, every undo unit with its byte
  copies, the undo array, the document and generation tables, and drops the
  notification and snapshot entries that belong to the kernel.
- `cdin_doc_close` frees the undo history and aborts an active transaction
  (DOC-030/DOC-031).
- An edit whose undo record cannot be recorded now fails with `LIMIT_EXCEEDED`
  and changes nothing (UNDO-031) instead of silently losing undo history.
- The undo memory cap accounts per document and evicts oldest-first, freeing
  the units it evicts and keeping its own accounting correct when units are
  coalesced; a new edit frees the redo tail it discards (UNDO-003).
- `cdin_doc_serialize` reads `[0, len)` and implements the two-call pattern:
  `buf == NULL` reports the required size and returns `OK`.
- Conformance runner: registering a handle no longer copies 24 bytes out of a
  1-byte string literal (found by ASan as a global-buffer-overflow), and the
  allocator no longer forwards a non-literal format string (found by clang under
  `-Wformat=2`).

### Removed — P1
- `cdin_edit_result.blob_before` / `blob_after` and their lengths. One pointed
  into the caller's input buffer, the other into the undo store. ABI-008
  requires explicit ownership; the two-call replacement is proposed in
  `docs/abi.md` and lands in P4 with the request structs. UNDO-020/021
  are unimplemented, not silently broken.

### Added — P0: build, CI and conformance runner
- Portable `Makefile` (`CC ?= cc`), objects and binaries under `build/`,
  `-MMD -MP` dependencies, `asan`/`tsan`/`bench`/`fuzz-build`/`trace` targets.
- CI matrix: ubuntu gcc, ubuntu clang, windows MSYS2 UCRT64, plus ASan, TSan
  and a header-compiles-as-C-and-C++ job.
- Conformance runner rewritten: one tokenizer, per-operation key schemas,
  typed handle table, per-vector PASS/FAIL table, exit non-zero on failure.
- Strict allocator shared by the runner and the tests: checks sizes, catches
  double free and leaks, and can fail the Nth allocation or relocate every
  block to expose a stale-pointer read.
- `tests/test_p1.c`: 8 regression tests for initialisation, growth atomicity,
  relocate-safe edits, destroy, and allocation-failure sweeps.

### Added — K0: ABI skeleton
- Status codes, ABI version query, feature query
- Injected allocator struct; kernel create/destroy
- Document handle type (`cdin_doc`, 24-bit slot + 40-bit generation)
- Version type (`cdin_version`, starts at 0; `CDIN_ANY_VERSION`)
- Conformance vector format and runner

### Added — K1: Documents
- Document create (empty or from bytes), close, version query, length, read, serialize
- Creation-time limits: max documents, max document size
- Byte-exact preservation: no BOM stripping, no normalization, invalid UTF-8 kept

### Added — K2: Replace
- `cdin_doc_replace`: the single edit primitive
- Atomic version check (expected version; `STALE_VERSION` on mismatch)
- Edit result: new version, start, removed, inserted, new length
- Range validation; document size limit on edit
- Batch replace returns `CDIN_UNSUPPORTED`

### Added — K3: Regions, lines, positions
- Region classification (valid/raw) derived from content
- UTF-8 validation
- Line index: `line_count`, `line_start`, `line_from_offset`
- LF, CRLF-as-one-unit, CR support; correct after edits
- Position validity: `check_position` (POS-020, POS-021)
- Conversions: byte<->code point, byte<->line+col

### Added — K4: Undo, redo, transactions
- Linear undo/redo history; undo and redo are new versions
- Coalescing by caller key
- New edit discards redo units
- Empty undo/redo returns `INVALID_STATE`
- Transactions: begin, commit, abort, replace inside
- `BUSY` for other writers during transaction; nested begin rejected
- Clean marker and metadata value

### Added — K5: Change notifications
- Per-subscriber bounded queue
- Subscribe returns current version
- Events in version order; each exactly once
- Overflow becomes `RESYNC` marker
- Closed event on document close
- Wakeup callback that only signals

### Added — K6: Concurrency, snapshots, shutdown
- Immutable snapshots with acquire/release; per-document limit
- Snapshot reads never see partial mutations
- FIFO sequence number assignment
- Shutdown: graceful and abort modes

### Added — K7: Search
- Literal and bytes modes; regex returns `CDIN_UNSUPPORTED`
- Forward and backward search; find-all with limit
- Results bound to document version
- ASCII-only case-insensitive (documented)
- Multiline literal search
- Invalid UTF-8 handling
- Persian/Arabic letters never treated as equal

### Added — K8: Regex spike
- Evaluated PCRE2 against ADR-0009 criteria: passes all
- Oniguruma not installed; not evaluated
- Recommendation: PCRE2 (BSD-3-Clause, C API, UTF-8, limits, cancellation)
- Engine not wired in; awaiting owner decision

### Added — K9: Unicode
- Pinned Unicode version: 15.1.0
- Unicode version query
- Simple case folding for case-insensitive search
- Grapheme boundary queries (code-point stepping; raw bytes each own grapheme)
- No normalization or mark-removal options

### Added — K10: Fuzzing, hardening
- Fuzz targets: public API inputs, region/line classifiers, vector parser
- Final header review against ABI rules
- Complete API documentation

### Added — K11: Performance
- Benchmark program covering all required operations
- Performance method documented
- No optimized paths added (awaiting baseline measurements)

### Known incomplete
- Regex engine not wired in (PCRE2 recommended)
- Batch replace returns `CDIN_UNSUPPORTED`
- Undo memory cap not enforced
- Blob return in edit result not implemented
- Full grapheme cluster rules not implemented
- Full Unicode case folding not implemented
- Fuzz targets not run
- Benchmarks not run
- No verification has been run on any requirement

### Blocker
- GCC (C:\msys64\mingw64\bin\gcc.exe v16.1.0) fails silently on this machine — exit code 1, no output, no object file. All verification is blocked.
