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

A release package includes `include/`, `lib/`, `bin/` when the platform needs it, the license, the README/changelog, documentation, a quickstart source file, build information, and a SHA-256 checksum. Packages are produced only after the source tests and installed-consumer smoke test pass on the same runner that built the artifact.
