# cdin-kernel

`cdin-kernel` is a portable C11 library for binary-safe text editing. It provides document handles, edits, undo/redo, transactions, immutable snapshots, text-position helpers, literal/byte search, and change notifications through a stable C ABI.

## Install

Download the package for your platform from the project's GitHub Releases page, extract it, and run the included installer:

- Linux/macOS: `./install.sh --prefix "$HOME/.local"`
- Windows: run `./install.ps1` in PowerShell. The release package targets 64-bit MinGW-w64 UCRT64.

The public header is `cdin/kernel.h`. In C or C++, include it with:

```c
#include <cdin/kernel.h>
```

After installation, compile a program with `pkg-config`:

```sh
cc app.c $(pkg-config --cflags --libs cdin-kernel) -o app
```

See the [user guide](docs/user-guide.md) for a complete example and [installation](docs/installation.md) for static/shared linking, custom prefixes, and release packages.

## Development workflow

The repository uses [PSX](https://github.com/m-mdy-m/psx) for its small C-library project rules, [Pipeline](https://github.com/m-mdy-m/pipeline) for the shared local/CI task graph, and [gix](https://github.com/m-mdy-m/gix) for branch flow. Run `psx check` and `pipeline run check` locally. After making the first Git commit on `main`, run `bash scripts/init-git-flow.sh` to initialize gix and create `develop`.

## Build instructions

### GNU/Linux

Install a C compiler and Make, then build and test:

```sh
sudo apt-get install build-essential cmake git pkg-config
make deps                 # installs the pinned m-mdy-m/log.c dependency locally
make all shared quickstart
make check
sudo make install
```

Supported release architectures are x86-64 and ARM64.

### macOS

Install the Xcode Command Line Tools, CMake, and Git, then run:

```sh
make deps
make all shared quickstart
make check
sudo make install
```

Release packages are available for Intel (x86-64) and Apple silicon (ARM64).

### Windows (64-bit)

Install [MSYS2](https://www.msys2.org/) and use its **UCRT64** terminal. Install the MinGW-w64 GCC, Make, CMake, and Git packages, then run:

```sh
make deps
make all shared quickstart
make check
make PREFIX=/ucrt64 install
```

The published Windows package targets MinGW-w64 UCRT64; MSVC compatibility is not claimed. A 32-bit Windows build is not currently published.

### GNU/Linux with Docker

A multi-architecture SDK image is published to Docker Hub for `linux/amd64` and `linux/arm64`. Replace `bitsgenix` with the account that publishes the image:

```sh
docker pull bitsgenix/cdin-kernel:latest
docker run --rm -it -v "$PWD:/workspace" bitsgenix/cdin-kernel:latest
```

The image includes the public headers, `cdin-kernel` and `m-mdy-m/log.c` static libraries, GCC, Make, CMake, and `pkg-config`. Use it as a build environment for applications that link against `cdin-kernel`; it is not an application image. See [Container builds](docs/installation.md#7-container-builds).

## Examples and documentation

- [Examples index](examples/README.md)
- [C quickstart](examples/c/quickstart.c)
- [User guide](docs/user-guide.md)
- [C ABI reference](docs/abi.md)
- [Language bindings](docs/language-bindings.md)
- [Installation and build](docs/installation.md)
- [GitHub Release assets](docs/github-releases.md)
- [Architecture](docs/architecture.md)
- [Developer guide](docs/development.md)

## Third-party

- **C standard library** — provided by the target platform.
- **POSIX Threads (`pthread`)** — system library used on Unix-like builds; not bundled.
- **Unicode tables** — generated into the source tree from Python's `unicodedata` and the third-party `regex` module. These Python packages are only needed to regenerate the tables, not to build or run the library. See `tools/gen_unicode.py` for data provenance and limitations.
- **m-mdy-m/log.c** — pinned C11 logging library (MIT), installed separately into the local `.deps/` prefix by `make deps`; the kernel links the static library and exposes it as a private transitive dependency to static consumers. Its source is kept under `build/deps/`, never copied into `src/`.
- **Build tools** — a C11 compiler, Make, Git, CMake, `pkg-config`, and an archiver (`ar`) are required to build from source.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
