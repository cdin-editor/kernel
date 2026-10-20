# Examples

The examples show different ways to call `cdin-kernel`. The **C ABI** in `include/cdin/kernel.h` is the supported public interface. The small bridge under `examples/ffi/` is demonstration code for languages whose FFI tools struggle with C callbacks and nested structures; it is not a separate stable API.

Build the core and optional bridge from the repository root:

```sh
make all shared
make ffi-shared
```

## C

Build and run the minimal direct-ABI example:

```sh
make quickstart
```

See [`c/quickstart.c`](c/quickstart.c). It creates a kernel and document, replaces a byte range, reads the updated text, then releases its resources.

## Python

Requirements: Python 3.10+ and the native build toolchain.

```sh
make ffi-shared
python examples/python/quickstart.py
```

The example uses `ctypes`; no third-party Python package is required. Set `CDIN_FFI_LIB` to select a bridge library outside `build/`.

## Go

Requirements: Go with `cgo` enabled and a C compiler.

```sh
make ffi-shared
cd examples/go
go run .
```

The example's `#cgo LDFLAGS` use the repository's Linux-style build path. Adjust those flags for another OS or an installed bridge.

## JavaScript and TypeScript

Requirements: Node.js and npm.

```sh
make ffi-shared
cd examples/node
npm install
npm start
npm run start:ts
```

The JavaScript example uses Koffi and `Buffer` objects so explicit byte lengths and embedded NUL bytes are preserved. TypeScript uses the same runtime bridge with compile-time types.

Each example prints `Hello CDIN` to standard output and reports the resulting document version to standard error. The native library and bridge must match the target operating system and CPU architecture.
