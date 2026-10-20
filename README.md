# cdin-kernel

`cdin-kernel` is a binary-safe text editing engine with a C ABI. It provides document lifecycle, byte-range edits, undo/redo, transactions, immutable snapshots, text/position helpers, literal and byte search, and bounded change notifications.

**Project status:** stable. The C ABI reports version **1.0.0** and is synchronized with CDIN Spec **1.0.0**. ABI-major changes are breaking; compatible additions must preserve the published ABI 1 contract. Compile bindings against the shipped header and query the loaded library version at startup.

## Start here

```sh
make              # build the static library: build/libcdin_kernel.a
make quickstart    # compile and run the minimal C example
make check         # conformance vectors + C regression tests
make shared        # build a shared library for FFI consumers
make install       # install header + static/shared library + pkg-config file
make ffi-shared    # build the optional, small-ABI FFI example bridge
make bench-scale SIZE_MIB=125  # benchmark 125 MiB, 500 MiB, or 1024 MiB documents
```

The core public API is defined by the single public header: [`include/cdin/kernel.h`](include/cdin/kernel.h). The bridge in `examples/ffi/` is an example adapter and is **not** part of the stable kernel ABI. The public header defines the full frozen ABI 1.0.0 surface.

## Large-document benchmark

Run a single local measurement with `make bench-scale SIZE_MIB=125`, `500`, or `1024` (1 GiB). The benchmark measures kernel creation, document load, line-count/index construction, a literal no-match search, snapshot acquisition, and document-load throughput. It prints one CSV row so it can be collected by CI.

The GitHub Actions workflow [Large-document benchmarks](.github/workflows/benchmark.yml) runs all three sizes on an Ubuntu 24.04 hosted runner, writes a table to the Actions run summary, and uploads per-size CSV artifacts. The measurements are indicative: runner load, CPU model, and memory pressure affect results, so compare runs on the same runner class and do not treat one run as a universal performance guarantee. The 1 GiB case needs several GiB of available RAM.

Release packages are built and tested on Linux x86-64/ARM64, macOS x86-64/ARM64, and Windows x86-64 (MinGW-w64 UCRT64).

## Documentation

- [Documentation home](docs/index.md)
- **[User guide](docs/user-guide.md)** — recommended first read; lifecycle, reading, editing, versions, undo/redo, transactions, snapshots, search, notifications, and text positions.
- [Getting started: build and link](docs/getting-started.md)
- [Installation and packaging](docs/installation.md) — install targets, staged installs, `pkg-config`, and release archives.
- [C ABI reference and compatibility rules](docs/abi.md)
- [Using from Python, Go, JavaScript, and TypeScript](docs/language-bindings.md)
- [Limits, statuses, and current support](docs/limits-and-status.md)
- [Architecture and ownership](docs/architecture.md)
- [Developer guide: tests, vectors, and change workflow](docs/development.md)
- [Conformance vector format](conformance/FORMAT.md)

## Feature and behavior notes

- Text is stored as length-delimited **bytes**. UTF-8 is interpreted for positions/search, but input bytes are never normalized, BOM-stripped, or NUL-terminated by the kernel.
- Use `expected_version` on mutations to detect stale edits. New documents begin at version `0`.
- The caller supplies allocator callbacks when creating a kernel. A lock vtable is optional; without one, use a kernel from one thread at a time.
- Regex search is not implemented and returns `CDIN_UNSUPPORTED`. `CDIN_RELOAD_KEEP_HISTORY` is also not supported.
- Default resource limits are declared in `include/cdin/kernel.h` and explained in [limits and status](docs/limits-and-status.md).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
