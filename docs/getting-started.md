# Getting started

This page takes a new user from a source checkout to a running document edit. For the complete conceptual walkthrough, see the [user guide](user-guide.md).

## Requirements

- A C11 compiler (`cc`, GCC, or Clang).
- GNU Make or a compatible `make` implementation.
- An archiver (`ar`) and ranlib for the static archive, normally shipped with the compiler toolchain.
- Python, Go, or Node.js is needed only for its corresponding example.

The core has no separate package-manager dependency. The examples that call the core through C FFI build a small native library first.

## Build the static library

From the repository root:

```sh
make
```

The output is `build/libcdin_kernel.a`. The one public header is `include/cdin/kernel.h`. There is no `make install` target or prebuilt release package in this repository; link the generated artifact from your application or create a packaging step appropriate for your environment.

## Compile and run the direct C example

```sh
make quickstart
```

This builds `examples/c/quickstart.c`, creates a kernel, creates a document, replaces the bytes for `world` with `CDIN`, reads the resulting bytes, and destroys the kernel. The expected stdout is:

```text
Hello CDIN
```

The example is intentionally explicit about the required allocator callbacks and the version passed to `cdin_doc_replace`.

To link your own program on a typical Unix-like system:

```sh
cc -std=c11 -Iinclude your_app.c build/libcdin_kernel.a -o your_app
```

If your application supplies pthread-based lock callbacks, also link with the platform's pthread option (typically `-pthread`). The core itself does not force the application to use pthreads.

## Build a shared library

For C FFI consumers and dynamic-language integrations:

```sh
make shared       # Linux: build/libcdin_kernel.so; macOS: build/libcdin_kernel.dylib; MinGW: build/cdin_kernel.dll
make ffi-shared   # matching shared library plus the examples/ffi convenience wrappers
```

The FFI bridge artifacts are named `build/libcdin_ffi.so`, `build/libcdin_ffi.dylib`, or `build/cdin_ffi.dll`, according to the build platform. The bridge wraps the core with a small set of functions and a default `malloc` allocator. It is supplied as example code, is not the stable public ABI, and must be rebuilt whenever the core version changes.

On Windows, use a compiler toolchain supported by your `make` environment (for example, MinGW-w64). The shared-library rule exports the public C ABI when `CDIN_KERNEL_BUILD_DLL` is defined.

## Run the test gates

```sh
make check
```

This command builds the C runner and checks every `.vec` file under `conformance/vec/`, then runs the C regression tests. To run only part of the suite:

```sh
make check-vectors  # conformance vectors; stops at first failed file
make vectors        # report every vector file; failures do not stop the loop
make check-tests    # C regression and concurrency tests
make asan           # AddressSanitizer + UndefinedBehaviorSanitizer (supported toolchain)
make tsan           # ThreadSanitizer (supported toolchain)
```

Sanitizer targets require a compiler/runtime that supports the selected sanitizer. Their shell behavior is POSIX-oriented, even though ordinary compilation targets also account for Windows.

## Next step

Read the [user guide](user-guide.md) before relying on offsets, versions, transaction visibility, or notification behavior. Those rules matter more than the mechanics of linking the archive.
