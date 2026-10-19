# Installation and packaging

`cdin-kernel` currently ships as source code. The repository has build targets for static/shared libraries, but **does not provide `make install`, a package-manager formula, or a prebuilt binary release**. This page explains how to stage the artifacts after building them. Packaging maintainers should add platform-appropriate versioning, install names, and upgrade rules before distributing a shared library to system-wide locations.

## 1. Build the artifact you need

Run from the repository root:

```sh
make                 # build/libcdin_kernel.a
make shared          # build/libcdin_kernel.so / .dylib / .dll
make ffi-shared      # optional example bridge used by the non-C examples
make check           # verify behavior before packaging
```

The public core requires only `include/cdin/kernel.h` plus the corresponding library. The `examples/ffi/` bridge is optional and is not a stable part of the kernel ABI.

## 2. Stage a Unix-like installation

For a local app or development prefix, stage only the files you use. Example for a POSIX environment with write access to `/usr/local`:

```sh
sudo install -D -m 0644 include/cdin/kernel.h /usr/local/include/cdin/kernel.h
sudo install -D -m 0644 build/libcdin_kernel.a /usr/local/lib/libcdin_kernel.a
```

To stage the Linux shared library instead of (or as well as) the static archive:

```sh
sudo install -D -m 0755 build/libcdin_kernel.so /usr/local/lib/libcdin_kernel.so
```

For the optional bridge, also stage `examples/ffi/cdin_ffi.h` if compiling against its declarations and `build/libcdin_ffi.so` if loading it at runtime. The Python, Go, and Node examples do not require the public header at runtime; they do require the bridge library.

For development without a system-wide install, keep the artifacts under `build/` and set the language example's `CDIN_FFI_LIB` environment variable to the absolute path of the built bridge. On Linux, the dynamic loader may also need a configured library search path, such as `LD_LIBRARY_PATH` for local testing. Prefer an application-local library bundle or a proper package configuration for production.

The Makefile does not currently assign versioned SONAME/install names or create symlink chains for system upgrades. A distro package should add that policy deliberately rather than inventing compatibility from the `0.2.0` draft ABI number.

## 3. Windows / MinGW

Build with a compatible MinGW-w64 toolchain and GNU Make. The generated files are normally:

- `build/libcdin_kernel.a`: static archive;
- `build/cdin_kernel.dll`: core dynamic library;
- `build/libcdin_kernel.dll.a`: import library for linking to the DLL;
- `build/cdin_ffi.dll` and `build/libcdin_ffi.dll.a`: optional bridge and its import library.

At runtime, place the required DLL next to the executable or in a directory included in `PATH`. At compile time, include `include/` and link the matching static archive or import library. Do not link a `.dll.a` from a different build or ship a DLL without the exact compatible interface declarations.

The Makefile targets are designed around GCC/Clang-style command-line flags. An MSVC build requires an equivalent project/build configuration and has not been verified by this repository's Makefile.

## 4. Runtime loading from other languages

Build the bridge:

```sh
make ffi-shared
```

Then point the example to the bridge location as appropriate for the shell:

```sh
# Linux / macOS
CDIN_FFI_LIB=/absolute/path/to/libcdin_ffi.so python examples/python/quickstart.py
```

```powershell
# Windows PowerShell
$env:CDIN_FFI_LIB = 'C:\path\to\cdin_ffi.dll'
python examples/python/quickstart.py
```

Go's cgo linker flags are compile-time settings in `examples/go/main.go`, so changing `CDIN_FFI_LIB` alone does not change its link search path. Edit the example's `#cgo LDFLAGS` for the target platform and install location. Node's Koffi example reads `CDIN_FFI_LIB` at runtime.

See [language bindings](language-bindings.md) for full build/run examples and [getting started](getting-started.md) for the native build and test targets.

## 5. Version and release discipline

The public header currently reports ABI `0.2.0` and the project remains draft. For every application/package release:

1. Build the native library from the same source revision as the header.
2. Run `make check` before packaging.
3. Include the license and preserve the package's source/version metadata.
4. Keep static, shared, import, and bridge artifacts from the same build together.
5. Rebuild foreign-language bindings when the header or binary ABI changes.
6. Do not treat ABI `0.x` as a promise of binary compatibility across arbitrary revisions.
