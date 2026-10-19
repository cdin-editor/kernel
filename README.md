# cdin-kernel

`cdin-kernel` is a binary-safe text editing engine with a C ABI. It provides document lifecycle, byte-range edits, undo/redo, transactions, immutable snapshots, text/position helpers, literal and byte search, and bounded change notifications.

**Project status:** draft. The ABI reports version **0.2.0**. Until the project declares a stable release, rebuild language bindings against the matching header and library rather than assuming binary compatibility across revisions.

## Start here

```sh
make              # build the static library: build/libcdin_kernel.a
make quickstart    # compile and run the minimal C example
make check         # conformance vectors + C regression tests
make shared        # build a shared library for FFI consumers
make install       # install header + static/shared library + pkg-config file
make ffi-shared    # build the optional, small-ABI FFI example bridge
```

The core public API is defined by the single public header: [`include/cdin/kernel.h`](include/cdin/kernel.h). The bridge in `examples/ffi/` is for examples and is **not** part of the stable kernel ABI.

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
