# Installation and packaging

`cdin-kernel` provides a C11 source build, a static library, a shared library, a public header, and a `pkg-config` file. The ABI is still marked **draft 0.2.0**; treat pre-release binaries as version-matched and rebuild bindings when the ABI changes.

## Build and test

From the repository root:

```sh
make clean
make all shared quickstart
make check
```

`make all` builds `build/libcdin_kernel.a`. `make shared` builds the platform library (`.so`, `.dylib`, or `.dll`) and import library where applicable. `make check` runs the conformance vectors and C regression/concurrency tests. The `examples/ffi/` bridge is optional and is not part of the core ABI.

## Install to a prefix

On Linux and macOS, install to the default `/usr/local` prefix (permission to write there may be required):

```sh
make all shared
sudo make install
```

For an unprivileged, project-local install, use a prefix owned by your user:

```sh
make all shared
make PREFIX="$HOME/.local" install
```

The installation contains:

- `include/cdin/kernel.h` — the only public core header;
- `lib/libcdin_kernel.a` — static library;
- a versioned shared library and linker symlink(s) on Linux/macOS;
- `lib/pkgconfig/cdin-kernel.pc` — `pkg-config` metadata;
- on Windows, `bin/cdin_kernel.dll` and `lib/libcdin_kernel.dll.a` alongside the static library.

The install recipe supports `DESTDIR` for package staging. This is useful for package managers and does **not** write into the real system directories:

```sh
make DESTDIR="$PWD/stage" PREFIX=/usr/local install
```

The staged files will be under `stage/usr/local/`. Remove only the files installed by this project with `make PREFIX="$HOME/.local" uninstall` (use the same `PREFIX`, `includedir`, `libdir`, `bindir`, and `DESTDIR` values used during installation).

## Windows with MSYS2 UCRT64

Use the UCRT64 shell and a MinGW-w64 GCC toolchain; do not build the Windows library with a Linux compiler. For example:

```sh
make clean
make CC=gcc all shared quickstart
make CC=gcc check
make CC=gcc PREFIX=/ucrt64 install
```

The Windows shared library is `bin/cdin_kernel.dll`; link-time consumers use `lib/libcdin_kernel.dll.a`. The static library is `lib/libcdin_kernel.a`. Keep the DLL beside your consuming executable or somewhere on the Windows DLL search path.

## Link from a C project

A static link can use the header and archive directly:

```sh
cc -std=c11 -I"$HOME/.local/include" app.c \
  "$HOME/.local/lib/libcdin_kernel.a" -o app
```

For a shared-library consumer, use the platform linker/rpath conventions for your install prefix. With `pkg-config` installed, query the exported flags with:

```sh
pkg-config --cflags --libs cdin-kernel
```

The package's `.pc` file is relocatable with the installed `include/` and `lib/` layout. On Windows, use the UCRT64 `pkg-config`/`pkgconf` environment and keep `bin/` available for DLL loading.

## Verify an installed copy

CI stages an installation under a temporary prefix, then compiles and runs the example twice: once against the installed static archive and once against the installed shared library/import library. The same check can be run locally:

```sh
make DESTDIR="$PWD/build/stage" PREFIX=/cdin install
STAGED_PREFIX="$PWD/build/stage/cdin"
CC=cc scripts/verify-installed.sh "$STAGED_PREFIX"
```

The smoke test verifies the installed header and linker artifacts, loads the shared library at runtime, and checks the consumer's observable output. It does not by itself establish full ABI compatibility or replace the test suite.

## Release archives

Each GitHub Release provides one archive per supported runner: Linux x86-64 (`.tar.gz`), macOS (`.tar.gz`), and Windows x86-64 with MinGW-w64 UCRT64 (`.zip`). Each archive includes the public header, static library, shared library/import files where supported, `pkg-config` metadata, docs, quickstart example, build information, and an installer (`install.sh` for Linux/macOS or `install.ps1` for Windows). An adjacent `.sha256` file is published for each archive.

For Linux/macOS, extract the `.tar.gz` and run `./install.sh`; by default it installs to `~/.local` without requiring root. Use `./install.sh --prefix /path/to/prefix` to choose a destination. For a system-wide install, choose a system prefix and use the appropriate privileges.

For Windows, extract the `.zip`, open PowerShell in the extracted directory, and run `./install.ps1`. The default destination is `%LOCALAPPDATA%\cdin-kernel`; `-Prefix` changes it and `-AddBinToUserPath` optionally adds the DLL directory to the user's PATH. The Windows package targets MinGW-w64 UCRT64 and does not claim MSVC ABI compatibility.

`BUILD-INFO.txt` records both the package version and the C ABI version. They are tracked separately; for example, release package `0.1.0` may contain the draft C ABI `0.2.0`. Release packages are attached to the GitHub Release only after the source tests, installed-consumer smoke test, and archive-install test pass on the runner that built each artifact.
