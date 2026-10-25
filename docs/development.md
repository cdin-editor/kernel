# Developer guide

This document is for contributors changing `cdin-kernel` itself: what lives
where, how to extend the ABI safely, and which gates a change must pass.

The behavioral reference for contributors is `docs/user-guide.md` (what the
public ABI does) and `docs/architecture.md` (how it does it). This guide's
value is process: layout, build loop, gates, and the cost of a change.

## 1. Repository layout

| Path | Contents |
|---|---|
| `include/cdin/kernel.h` | The public ABI. Authoritative for every type, constant and default. |
| `src/` | Implementation, one module per concern (see 2). An opaque single `internal.h` share all internal state. |
| `src/api/` | Thin wrappers: lock/unlock, argument validation, use of the modules. No policy. |
| `conformance/` | Vector runner (`runner/`), vector files (`vec/<family>/`), the format description (`FORMAT.md`). |
| `tests/` | C regression tests: `test_p1.c` (functional + allocation-failure atomicity), `test_p3_conc.c` (concurrency), `fuzz_*.c` (libFuzzer harnesses). |
| `bench/` | Benchmark programs (`bench.c`, `bench_scale.c`). |
| `fuzz/` | `fuzz_ops.c`: standalone op-sequence fuzzer (works without libFuzzer via `-DSTANDALONE`). |
| `support/` | `strict_alloc.{h,c}`: counting/failing allocator used by tests, vectors, bench. |
| `examples/` | `c/quickstart.c`, `ffi/` bridge, `go/`, `node/`, `python/` binding examples. |
| `tools/` | `gen_unicode.py` (regenerate Unicode tables), `trace.py` (requirement trace report). |
| `packaging/` | `.pc` template, release-install documentation and scripts. |
| `docs/` | The documentation you are reading. |

## 2. Source map

```
src/
  kernel.c          kernel lifecycle, shutdown, locking entry, queries
  internal.h        shared internal declarations and module ownership map
  util/             mem.c, utf8.c, unicode_tables.c/h
  core/             block.c (refcounted, copy-on-write text), handle_table.c
  text/             lines.c, positions.c, graphemes.c
  search/           literal.c, fold.c
  history/          undo.c (units, clean marker, eviction, blobs)
  doc/              doc.c (table + lifecycle), edit.c (replace/batch/undo/accept)
  txn/              txn.c
  notif/            notif.c, wake_list.c
  api/              one wrapper per public group (doc/edit/text/search/txn/notif/snapshot)
```

Module rule: policy lives in the module (`doc/`, `txn/`, ...); `src/api/`
only performs lock, argument validation, and callback dispatch. If a public
behavior is hard to find, check whether it ended up in the wrong layer.

## 3. Build loop

```sh
make                # release build of the library
make check          # vectors + tests: the change is not done until this is green
make debug          # -O0 -g3
make asan           # ASan+UBSan over the whole vector and test set
make tsan           # TSan ditto (needs a POSIX shell; run under WSL/CI on Windows)
make bench          # performance current numbers
```

On Windows/MinGW, MSYS2/UCRT64 provides the toolchain. `CC`, `AR`, `RANLIB`
can be overridden on the command line. The public header is picked up in
`include/` for every compile (no generated-code step).

Do not run two `make` processes on one checkout; object files race and the
link fails with confusing `undefined reference` lists. A `make clean && make`
cures the corrupted state.

## 4. Verification gates

A change is complete when every gate that applies is green *in this
checkout*:

| Gate | Command | What it verifies |
|---|---|---|
| Compile cleanliness | `make` | `-Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wformat=2 -Wpedantic -Werror` |
| Golden behavior vector | `make check-vectors` | Every documented behavior that the spec fixes, as data. |
| C regression tests | `make check-tests` | Functional + allocation-failure atomicity (`test_p1`), concurrency (`test_p3_conc`; uses pthreads). |
| Memory errors | `make asan` | Every vector and test under AddressSanitizer + UBSan. |
| Data races | `make tsan` | Vector and test set under ThreadSanitizer. |
| Performance | `make bench`, `make bench-scale` | Compare against baseline; investigate >10% regressions. |
| Fuzz | `make fuzz-build` (clang) or `build/fuzz_ops` standalone | Years of ABI/parse/op-sequence robustness; run before releases. |

Sanitizer/fuzz targets need clang or a POSIX environment; on Windows they
were last run in WSL2 with the same checkout copied into `/tmp`.

Report format: state which gates were run in this change, in this checkout,
and which were not (`NOT RUN`), with reasons. Do not report a green `make
asan` unless it printed the green gate line.

## 5. Extending the public ABI

A new public function or option is an ABI event. The process:

1. **Decide the shape first.** Draft the declaration in
   `include/cdin/kernel.h`, including the struct fields, status values, and
   enum positions it will need. New values only append.
2. **Bump the ABI minor** unless it is a pure addition behind an
   already-reserved value. Update `conformance/vec/abi/abi.basic.vec` (it
   pins the version triple and the feature mask).
3. **Implement** in the owning module; add a `src/api/XXX_api.c` wrapper if
   a new group; follow the atomicity discipline (see architecture
   5): validate, accept, prepare fallible allocations, apply unfailingly.
4. **Document in vectors**, in the user guide, and in the ABI reference in
   the same change. A function without vector coverage is unusable.
5. **Run the full gate** and report it.

Struct-with-`struct_size` growth rules live in docs/abi.md section 9; follow
them exactly. Never reorder fields, never reuse a reserved value with a new
meaning, and do not add optional fields to locked-size sub-tables
(`cdin_allocator`, `cdin_lock_vtable`, `cdin_log_sink`) without an owner
decision, as that needs a new contract layout.

## 6. Extending the vectors

Read `conformance/FORMAT.md` first. The essentials:

- A vector is a line-based script: `vector name covers REQ-ID...`, ops, and
  `expect STATUS key=value...` assertions.
- Every vector restarts with a fresh kernel state and a counting allocator
  that can be told to fail the n-th allocation.
- One file, one topic; the runner process model is per-file.
- When the vector grammar grows a new operation, update `FORMAT.md` in the
  same commit.
- Never change a vector's expectation to "fit" a code change: the pointers
  are the contract. If a behavior is deliberately newly decided, the vector
  change is a separate, explicit commit recorded in the phase notes.

`make trace` regenerates the requirement-to-vector table
(`tools/trace.py`); run it when a vector adds or drops a requirement cover.

## 7. Style rules

The existing codebase is C11 with warnings-as-errors. Match the style of
neighboring files exactly:

- `snake_case` functions, `CamelCase` for types, prefixed public symbols
  (`cdin_`), prefixed internal module symbols (`doc_`, `txn_`, `notif_`).
- Every allocation goes through `k_alloc`/`k_realloc`/`k_free` (the kernel's
  allocator). No `malloc`/`free` anywhere.
- Fallible calls return `cdin_status`; document out-parameters written on
  success. `cdin_status` values never hard-code; use library constants.
- One exit label per function only where cleanup chains make it clearer;
  the existing codebase embraces early returns inside `api/` wrappers but
  keeps fault atomicity inside each module's own validation/preparation.
- Comments explain non-obvious invariants and none of the obvious. No
  commented-out code. No TODO without an owner and a spec link.
- Every error message string the log sink could receive is UTF-8 and
  NUL-terminated internal buffers (`k_log` computes the length).

## 8. Testing additions

Every public function needs:

- at least one `expect CDIN_OK` vector for the happy path,
- one vector per documented failure status,
- coverage in an allocation-failure sweep (`test_p1.c`) when it allocates,
- a concurrency test (`test_p3_conc.c`) when two threads could interact
  with anything beyond the one document under the lock,
- a `notif.*` vector when it queues subscriber events,
- a `history`-cap or `blob` test if it alters any accounting.

When you "just change" validation order, hunt down tests that blessed the
old order. Validation order is documented as sequence, so vector files that
pin the order matter.

## 9. Performance work

- Replace `bench/bench.c` baselines with a phase report when the numbers
  move by more than ~10%; otherwise note new numbers in the report only.
- Move work only when measurements justify it; the library's contract puts
  simplicity and memory bounds before micro-optimization.
- `bench-scale` builds a 125/500/1024 MiB document (overwrite `SIZE_MIB`)
  and reports times for long documents, which is where silent overshoot in
  history or line-index building shows up first.

## 10. Release checklist

1. Update `include/cdin/kernel.h` macros: ABI (if it changed) and either
   version of the public header's comment block.
2. Bump `conformance/vec/abi/abi.basic.vec` to expect the new triple and
   mask, or adjust it if the mask changed membership.
3. Run: `make check`, `make asan`, `make tsan`, `make bench`, `make
   bench-scale`, `make quickstart`.
4. Regenerate Unicode tables with `tools/gen_unicode.py` only after
   re-verifying the triple and feature names it emits.
5. Build the release archives (per `packaging/RELEASE-INSTALL.md`), verify
   intaller hashes and that `install.sh`/`install.ps1` accept `--prefix`.
6. Record everything in the phase report: gates, numbers, NOT RUN list.
