# Developer guide

This document is for changing the kernel, extending the API, or validating a release candidate. Read the [architecture](architecture.md) first, and use the public header—not old phase notes—as the API contract.

## Source tree map

| Path | Purpose |
|---|---|
| `include/cdin/kernel.h` | The only public kernel header. |
| `src/kernel.c` | Kernel lifecycle, feature/version queries, lock and log helpers. |
| `src/api/` | Public API wrappers and argument/result validation. |
| `src/core/` | Text-block ownership, copy-on-write support, generation handles. |
| `src/doc/` | Document lifecycle and content mutation. |
| `src/txn/` | Transaction lifecycle. |
| `src/history/` | Undo/redo accounting, clean marker, coalescing, eviction. |
| `src/text/` | UTF-8 position helpers, line positions, grapheme stepping. |
| `src/search/` | Literal/byte scanning and case-fold handling. |
| `src/notif/` | Bounded notification queues and wakeups. |
| `src/util/` | Allocation helpers, UTF-8 mechanics, Unicode tables. |
| `conformance/vec/` | Executable, declarative behavior examples. |
| `conformance/runner/` | Vector parser and runner. |
| `tests/` | C regression, allocation-failure, concurrency, and fuzz entry points. |
| `fuzz/`, `support/` | Fuzz targets and strict allocator support. |
| `bench/` | Benchmark programs. |
| `tools/` | Unicode-table generation utilities. |
| `examples/` | User examples and optional FFI adapter. |

## Build targets

Run from repository root.

```sh
make             # build static archive
make check       # vector gate + C regression tests
make check-vectors
make vectors     # report all vectors, without stopping after the first vector file failure
make check-tests
make quickstart  # compile and execute direct C usage example
make shared      # position-independent shared core library
make ffi-shared  # shared core plus examples/ffi/cdin_ffi.c
make asan        # ASan + UBSan target (toolchain/platform dependent)
make tsan        # TSan target (toolchain/platform dependent)
make fuzz-build  # libFuzzer-oriented targets; normally use Clang
make bench       # build benchmark target
make trace       # print vector trace rows
make clean
```

`make check` is the acceptance gate. Do not use `make vectors` as the only test: that target is intended to collect a full report and deliberately ignores failures in the loop.

## Conformance vectors

A vector is an executable statement of behavior. Store vectors under `conformance/vec/<family>/` and read [the vector format](../conformance/FORMAT.md) before adding one. A good change adds a small vector for the public behavior plus regression tests for memory/failure/concurrency implications.

Recommended loop:

1. Identify the public rule and its owning module.
2. Add or update the vector first where possible.
3. Implement the smallest change in the subsystem that owns the behavior.
4. Run the nearest vector file through `build/conformance-runner path/to/file.vec` while iterating.
5. Run `make check` after the implementation is complete.
6. Run `make asan` or `make tsan` where the change touches allocation, ownership, mutation, callbacks, or concurrency.
7. Update user/ABI/developer docs and examples in the same change.

Each vector file runs in a separate runner process under `make check-vectors`; this makes each case independent of process-level residue and prevents a failure from contaminating later files.

## Failure atomicity and allocator correctness

All kernel-owned memory must use the configured allocator helpers; do not call `malloc`, `realloc`, or `free` directly from a core subsystem. Keep allocation sizes exact: every free and realloc old-size value must match the actual block size. Plan every potentially failing allocation before publishing the new document state. If a mutation fails before publication, the bytes, version, and history must remain unchanged.

The strict allocator in `conformance/runner/alloc_strict.c` and `support/strict_alloc.c` is intended to catch size mismatches and leaks. Add or adapt failure-point sweeps for any new multi-allocation operation.

## Public ABI changes

The public header is a contract, not just a compiler interface. Before changing it:

- Add/update the relevant `struct_size`, flag, ownership, threading, and failure semantics comments.
- Decide whether the change can be additive without changing existing struct layout or enum meaning.
- Update ABI docs, feature reporting, and language-bridge wrappers/examples.
- Check all callers in `tests/`, `conformance/runner/`, `bench/`, and `examples/`.
- Never silently reinterpret a reserved nonzero flag; reject it until its behavior is fully implemented and tested.

The ABI 1.0.0 contract is stable. Changes to major-version layouts, widths, constant meanings or prototypes are breaking; additive evolution must follow the documented compatibility rules and conformance tests.

## Threading and callbacks

Do not introduce a global mutex or hidden thread of execution: the caller-supplied lock vtable defines synchronization. Every public API must use the standard lock/wrapper pattern. Keep wakeup callbacks outside the critical section using the existing wake-list path. Review re-entrancy, callback lifetime, shutdown, and kernel destruction whenever adding a callback or worker-facing object.

## Unicode changes

Do not hand-edit generated Unicode tables without documenting the source/version. Keep the runtime version query truthful. Test valid multibyte sequences, malformed sequences/raw bytes, CRLF boundaries, Unicode case folds, and grapheme rules. Byte-preserving behavior is a core invariant; do not normalize or replace document bytes.

## Fuzzing and benchmarks

Use `make fuzz-build` with a libFuzzer-compatible Clang toolchain when changing parser/ABI/classifier logic. Add a regression vector for every fuzz-discovered bug. `make bench` builds the benchmark, but performance claims should include the machine/toolchain, corpus, and exact command; benchmark results are not correctness evidence by themselves.

## Change checklist

- [ ] Behavior has a conformance vector or a clear reason it cannot be vector-tested.
- [ ] `make check` passes.
- [ ] Relevant sanitizer/failure-sweep tests pass, where available.
- [ ] No core allocation bypasses the injected allocator.
- [ ] Failure does not partially change text/history/version.
- [ ] Callback/threading and shutdown behavior are reviewed.
- [ ] Public header, README, docs, and examples are mutually consistent.
- [ ] Unsupported behavior is explicitly documented rather than implied by an enum.
