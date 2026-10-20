# Getting started

This guide builds the library, installs it, and shows the expected include/link pattern for a C application. See the [user guide](user-guide.md) for API behavior and lifecycle details.

## Requirements

- A C11 compiler (GCC or Clang).
- GNU Make, an archiver (`ar`), and `ranlib` (normally provided by the compiler toolchain).
- `pkg-config` is recommended for consuming an installed library.

The core has no external runtime-library dependency. On Unix-like systems, the build links with the platform's POSIX thread library.

## Build and test the library

From the repository root:

```sh
make clean
make all shared quickstart
make check
```

This creates `build/libcdin_kernel.a` and a platform shared library: `.so` on Linux, `.dylib` on macOS, or `.dll` plus an import library on Windows. The `quickstart` target compiles and runs `examples/c/quickstart.c`; its standard output is:

```text
Hello CDIN
```

## Install

Install into `/usr/local` (administrator permissions may be required):

```sh
sudo make install
```

Or install under your home directory without root access:

```sh
make PREFIX="$HOME/.local" install
```

The install includes:

- `include/cdin/kernel.h` — the public C ABI header;
- `lib/libcdin_kernel.a` — static library;
- a versioned shared library and linker symlinks where supported;
- `lib/pkgconfig/cdin-kernel.pc` — metadata for `pkg-config`;
- on Windows, the DLL and its import library.

Use `DESTDIR` to stage an install for packaging or testing without writing to the final prefix:

```sh
make DESTDIR="$PWD/stage" PREFIX=/usr/local install
```

## Use the installed library

Include the public header with angle brackets. This is the same include form an installed consumer should use; do not include source-tree paths in your application:

```c
#include <cdin/kernel.h>
```

Compile and link with `pkg-config`:

```sh
cc -std=c11 app.c $(pkg-config --cflags --libs cdin-kernel) -o app
```

To link the static archive directly instead, pass the installed include directory and archive path to the compiler. For a shared-library application, ensure the platform loader can find the installed library at run time.

## Example lifecycle

The quickstart demonstrates the basic ownership pattern:

1. Create a kernel configuration and provide allocator callbacks.
2. Create a kernel and a document from a byte buffer.
3. Replace a byte range using an expected document version.
4. Read the document into caller-owned memory.
5. Close the document and destroy the kernel.

Always check each returned `cdin_status`. Document offsets and lengths are measured in bytes, not Unicode code points or grapheme clusters. Buffers returned by the core are copied into memory supplied by the caller.

## Platform notes

- **Linux:** install a C toolchain with your distribution's package manager.
- **macOS:** install the Xcode Command Line Tools.
- **Windows:** use MSYS2's UCRT64 environment with MinGW-w64 GCC. The published Windows artifacts target 64-bit UCRT64, not MSVC.
- **Docker:** a Linux SDK image is published for `linux/amd64` and `linux/arm64`; see [Docker SDK image](installation.md#docker-sdk-image).

For ABI stability, memory ownership, concurrency, and search semantics, continue with the [user guide](user-guide.md) and [ABI reference](abi.md).
