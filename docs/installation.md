# Installation and build

This document explains how to build `cdin-kernel` from source, install it,
link applications against the static or shared library, and how the release
packages are produced and consumed.

The kernel uses **m-mdy-m/log.c** as a separately installed C11 logging
library. Its source is pinned in `deps.lock`, cloned into `build/deps/`, and
installed into the project-local `.deps/` prefix by `make deps`. It is never
copied into `src/` or compiled as part of the kernel source list. The logger
is built static with position-independent code, so the kernel shared library
does not require a separate logger DLL/SO at runtime. Static consumers receive
the logger as a private transitive dependency through `cdin-kernel.pc`.

The build needs a C11 toolchain, GNU `make`, Git, CMake 3.16+, `pkg-config`,
and archiver tools. `awk`, `sed`, and an install binary are required by some
packaging targets; sanitizer runs need a POSIX shell.

## 1. Supported platforms

| Platform | Toolchain | Notes |
|---|---|---|
| Linux x86-64, arm64 | gcc or clang, glibc or musl | Full build incl. sanitizers; `-pthread` is used by the test suite only. |
| macOS x86-64 / arm64 | Apple clang | Shared library uses `@rpath`; see 5. |
| Windows x86-64 | MinGW-w64 (GCC) under MSYS2 UCRT64 | Produces `cdin_kernel.dll` + import lib. MSVC is not supported. |

The public kernel header and static library are portable C11. The external
logger is built for the host toolchain as a static archive; use the same
compiler/ABI for the kernel and dependency.

## 2. Build from source

### 2.1 Linux and macOS

```sh
sudo apt-get install build-essential cmake git pkg-config   # Debian/Ubuntu example
make deps                                           # install pinned logger locally
make all shared quickstart                        # build everything
make check                                        # run all gates
sudo make install                                 # default PREFIX=/usr/local
```

### 2.2 Windows (MSYS2 UCRT64)

```sh
pacman -S --needed make git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-pkgconf
make deps
make all shared quickstart
make check
make install PREFIX=/ucrt64/local        # or any MSYS path
```

Outside MSYS2 (a native shell), set `CC`, `AR`, and `RANLIB` if the defaults
are not on `PATH`:

```sh
make CC=C:/ProgramData/mingw64/mingw64/bin/gcc.exe \
     AR=C:/ProgramData/mingw64/mingw64/bin/ar.exe \
     RANLIB=C:/ProgramData/mingw64/mingw64/bin/ranlib.exe
```

`make` detects a Windows host by `OS=Windows_NT` and the `uname` output, so
POSIX shell logic is skipped for cmd.exe; MSYS2/UCRT64 shells behave like
POSIX. Never run two `make` invocations on the same checkout concurrently
(for example from two terminals): object files can interleave and corrupt the
link step. `make clean && make` recovers.

### 2.3 Make targets

| Target | Produces | Description |
|---|---|---|
| `deps` | `.deps/` | Clone, build, and install the pinned `m-mdy-m/log.c` static library. |
| `all` | `build/libcdin_kernel.a` | Static library only (requires `make deps`). |
| `shared` | `build/cdin_kernel.dll` (+ `.dll.a`) / `libcdin_kernel.so(.MAJOR)` / `.dylib` | Shared library with symbol visibility. |
| `ffi-shared` | `build/cdin_ffi.dll` `libcdin_ffi.so` | Optional demo bridge used by the language-binding examples. |
| `quickstart` | runs `build/quickstart` | Builds and executes the C example. |
| `check` | - | `check-vectors` + `check-tests`; the full quality gate. |
| `check-vectors` | - | Run every conformance vector, one process per file; stop on first failure. |
| `vectors` | - | Run every vector and report all failures (no early exit). |
| `check-tests` | - | Run `test_p1`, `test_p3_conc`, and the logger integration regression test. |
| `tests` | - | Alias of `check-tests`. |
| `asan` | - | Clean rebuild with ASan+UBSan, then the full vector+test set. |
| `tsan` | - | The same with the thread sanitizer. |
| `bench` | `build/bench` + run | Default benchmark table. |
| `bench-scale` | `build/bench-scale` + run | Large-document benchmark; `make bench-scale SIZE_MIB=125` (also 500, 1024). |
| `fuzz-build` | `build/fuzz_*` | libFuzzer-based harnesses; requires clang. |
| `trace` | stdout | Requirement-id traceability table derived from the vectors. |
| `install` / `uninstall` | - | Install header + libraries + pkg-config (see 4). |
| `clean` | - | Remove intermediate objects and outputs. |

Build variables: `CC`, `AR`, `RANLIB` (toolchain), `CFLAGS`, `LDFLAGS`,
`LDLIBS` (additional link flags), `DEPS_PREFIX` (logger install prefix; defaults
to `.deps/`), `PREFIX`, `DESTDIR` (installation layout),
`SIZE_MIB` (bench-scale input), `STANDALONE=1` (fuzz driver without
libFuzzer).

## 3. Build artifacts

| File | What it is |
|---|---|
| `build/libcdin_kernel.a` | static library for linking |
| `build/cdin_kernel.dll` + `build/libcdin_kernel.dll.a` | Windows DLL + import library |
| `build/libcdin_kernel.so.$(ABI_MAJOR)` (+ unversioned link) | Linux shared library with stable soname |
| `build/libcdin_kernel.dylib` (+ `.MAJOR` link) | macOS shared library |
| `build/conformance-runner` | vector runner (not installed) |
| `build/test_p1`, `build/test_p3_conc`, `build/test_logging` | regression tests (not installed) |
| `build/bench`, `build/bench-scale`, `build/fuzz_ops` | performance and fuzz helpers (not installed) |

The `.pc` file generated by `make install` contains the ABI version so that
`pkg-config` consumers always see the installed version.

## 4. Install layout

```sh
make install [PREFIX=/your/prefix] [DESTDIR=/staging]
```

| Destination | Content |
|---|---|
| `$(PREFIX)/include/cdin/kernel.h` | the single public header |
| `$(PREFIX)/lib/libcdin_kernel.a` | static library |
| `$(PREFIX)/lib/libcdin_kernel.so.$(M)` `.$(MAJOR)` `libcdin_kernel.so` | Linux shared chain (three names) |
| `$(PREFIX)/bin/cdin_kernel.dll`, `lib` import lib | Windows layout |
| `$(PREFIX)/lib/pkgconfig/cdin-kernel.pc` | `pkg-config` metadata with `Requires.private: m-mdy-m-log` |
| `$(PREFIX)/include/m-mdy-m/log.h`, `lib/libm-mdy-m-log.a` | separately installed logger dependency |
| `$(PREFIX)/share/licenses/m-mdy-m-log/` | dependency license and upstream notice |

`DESTDIR` supports staging (`DESTDIR=/tmp/stage make install`); paths written
into `cdin-kernel.pc` still use `PREFIX`, which is standard for package
builders.

## 5. Linking an application

### 5.1 Static

```sh
cc app.c $(pkg-config --cflags cdin-kernel) \
  "$(pkg-config --variable=libdir cdin-kernel)/libcdin_kernel.a" \
  $(pkg-config --libs --static m-mdy-m-log) -o app
```

Manual flags, no pkg-config (link the logger archive after the kernel archive):

```sh
cc -std=c11 -I/usr/local/include app.c \
   /usr/local/lib/libcdin_kernel.a /usr/local/lib/libm-mdy-m-log.a -o app
```

Windows (MinGW):

```sh
cc -std=c11 -I"C:/msys64/ucrt64/local/include" app.c \
   "C:/msys64/ucrt64/local/lib/libcdin_kernel.a" \
   "C:/msys64/ucrt64/local/lib/libm-mdy-m-log.a" -o app.exe
```

For static linking, use the `libcdin_kernel.a` archive path explicitly and link `m-mdy-m-log` after it. `Requires.private` makes `pkg-config --libs --static cdin-kernel` expose that logger dependency, but `--static` does not force the linker to choose the kernel `.a` instead of an installed shared library.

### 5.2 Shared

Linking works identically through `pkg-config` (`-lcdin_kernel` resolves the
import/soname library). Two runtime notes:

- Linux: the installed soname is `libcdin_kernel.so.$(ABI_MAJOR)`; the
  unversioned name is the development symlink. RPATH or `ldconfig` decides
  where the loader finds it, as usual.
- Windows: `cdin_kernel.dll` must be on the executable's search path (same
  directory, or `PATH`) in addition to having the import lib at link time.

### 5.3 Embedding without install

For vendored sources, include the repository in your build, or add the
checkout to your `CMakeLists.txt` / build system as a static library target
and `add_include_directories(kernel/include)`. There is no required
generated-code step; the library is built from the C sources directly.

## 6. Release packages

`packaging/RELEASE-INSTALL.md` documents the archive layout. Packages contain
the header, static and shared libraries, the pkg-config file, the example
programs, this documentation (`docs/` at the root and under
`share/cdin-kernel/`), `INSTALL.md`, `BUILD-INFO.txt` (which records the
package release version separately from the C ABI version), and an installer
script per platform:

```sh
# Linux/macOS archive
./install.sh --prefix "$HOME/.local"
# Windows archive (run in PowerShell)
./install.ps1 -Prefix "C:\Users\me\cdin" -AddBinToUserPath
```

Each release asset has a matching `.sha256` checksum file; verify before
unpacking and running the installer.

## 7. Container builds

The project publishes a container image containing the kernel and logger
headers/static libraries, the kernel shared library, GCC, Make, CMake, Git,
and pkg-config, so an editor project can compile
against the kernel without installing a toolchain locally. Mount your project
and compile inside it:

```sh
docker run --rm -v "$PWD:/workspace" -w /workspace \
    <registry>/<cdin-sdk-image> \
    cc app.c $(pkg-config --cflags cdin-kernel) \
  "$(pkg-config --variable=libdir cdin-kernel)/libcdin_kernel.a" \
  $(pkg-config --libs --static m-mdy-m-log) -o app
```

Consult the project README for the current image name; the image is built
from this same source tree, so its ABI version matches the release it was
built from.

## 8. Verify an installation

```sh
pkg-config --modversion cdin-kernel        # expected ABI version triple
cc quick.c $(pkg-config --cflags --libs cdin-kernel) -o quick && ./quick
```

A green run prints `version=1 length=11` and the edited document text; if
`pkg-config` is not used, the compile line from 5.1 works.

## 9. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `make: awk: not found` (or missing version in `.pc`) | POSIX tools missing on PATH (Windows native shells often lack awk) | Use MSYS2/UCRT64 shell for packaging-related targets; otherwise awk is only needed for `install` and pkg-config generation. |
| `undefined reference to cdin_*` at link | wrong library arch (32/64) or old stale `.a` linking | Rebuild with `make clean && make`; verify arch with `gcc -dumpmachine`. |
| Program exits with `DLL not found` on Windows | DLL not on the executable's PATH | Copy `cdin_kernel.dll` beside the exe or extend PATH. |
| Multiple `make` runs corrupt objects | parallel builds in one checkout | `make clean && make`; serialize invocations. |
| MSVC errors about `__declspec` | MSVC is not a supported build target | Use MinGW-w64; link the produced import lib from other languages via C ABI only. |
| pkg-config reports the wrong prefix | staged DESTDIR install but runtime pc copied somewhere else | use the `PREFIX` passed at install time for `PKG_CONFIG_PATH`. |

## 10. NOT RUN

The kernel build and its full conformance/regression suite were exercised in
this integration environment against a local test double implementing the
pinned logger's public API. This verifies CDIN's compile/link integration and
callback mapping, but it does **not** replace a build against the actual
upstream logger. The following were not exercised here: fetching/building the
pinned upstream commit from GitHub (network access was unavailable), a clean
`make install` and release package using the real dependency, container image
builds, libFuzzer (`fuzz-build`), and macOS/Windows linking.
