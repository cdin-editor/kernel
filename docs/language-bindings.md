# Language bindings: Python, Go, JavaScript, and TypeScript

The kernel's supported public boundary is a **native C ABI** described by [`include/cdin/kernel.h`](../include/cdin/kernel.h). The ABI is stable at `1.0.0`; ship the library, matching header, and bindings as one versioned unit, and query `cdin_abi_version` on startup. The examples below use the small bridge in `examples/ffi/` because it hides the allocator callback and the nested configuration structs. That bridge is sample code, **not part of the public kernel ABI**.

## Choose your integration

| Language | Recommended starting point | Notes |
|---|---|---|
| C / C++ | Include `cdin/kernel.h`; link `libcdin_kernel` | Use the direct ABI and provide allocator callbacks. C++ callers should include the C header in a C-compatible context; exported API is `extern "C"`. |
| Python | `ctypes` + `examples/ffi/cdin_ffi.h` | No generated extension is needed. Use explicit `argtypes` and `restype`; byte buffers must outlive the call. |
| Go | `cgo` + bridge | `cgo` needs a C compiler and the native library. Copy Go byte slices into C memory for the duration of the call; never retain a Go pointer in the kernel. |
| JavaScript | Node.js + Koffi | Koffi loads the native shared library and supports C-style declarations. Use `Buffer` for byte strings, not JS strings, when exact bytes matter. |
| TypeScript | Node.js + Koffi, with TS types around the JS wrapper | TypeScript types do not change the native ABI. Keep `bigint` for `uint64_t` values and use the same runtime bindings as JavaScript. |
| Other languages | Any FFI that can call C | The bridge deliberately uses scalar arguments, `void *`, integer handles, and byte pointers. The platform ABI and pointer width still matter. |

## 1. Build the native libraries

From the repository root:

```sh
make                 # static core: build/libcdin_kernel.a
make shared          # shared core: build/libcdin_kernel.so or platform equivalent
make ffi-shared      # shared core + optional bridge: build/libcdin_ffi.so or equivalent
make check           # conformance vectors and C tests
```

On Windows the shared output is a `.dll`; on macOS it is a `.dylib`. The examples look for the bridge at the platform default path under `build/`. Set `CDIN_FFI_LIB` to the full path when you install it elsewhere.

The target names and output filenames are declared by the Makefile. On platforms without a compatible Make toolchain, compile the same source files with your platform's C compiler and link the platform thread/runtime libraries as needed. Do not copy a Linux `.so` into a Windows or macOS build.

## 2. Python (`ctypes`)

Requirements: Python 3.10+ is a practical baseline for the example, a C compiler, and the project build dependencies. No third-party Python package is required.

```sh
make ffi-shared
python examples/python/quickstart.py
```

Expected stdout:

```text
Hello CDIN
```

The example writes the document version to stderr. To load an installed library, pass its path:

```sh
# Linux/macOS shells
CDIN_FFI_LIB=/absolute/path/to/libcdin_ffi.so python examples/python/quickstart.py
```

On Windows PowerShell:

```powershell
$env:CDIN_FFI_LIB = 'C:\path\to\cdin_ffi.dll'
python examples/python/quickstart.py
```

Important `ctypes` rules:

- Declare every function's `argtypes` and `restype`. Otherwise pointers and 64-bit values can be truncated or misinterpreted.
- The bridge returns statuses as integers. `0` is `CDIN_OK`; use `cdin_ffi_status_name` to turn errors into readable diagnostics.
- `bytes` are passed as `uint8_t *` plus an explicit length. Embedded NUL bytes are allowed.
- The size-query read is intentional: call once with a null buffer, allocate the returned size, then read again.
- Always close documents and destroy the kernel, including on exceptions. The example uses `try/finally`.

## 3. Go (`cgo`)

Requirements: Go, a C compiler supported by `cgo`, and a built shared bridge.

```sh
make ffi-shared
cd examples/go
go run .
```

Expected stdout is `Hello CDIN`; the version goes to stderr. If your linker cannot find the bridge, set `CDIN_FFI_LIB` is **not** automatically consumed by the Go linker: adjust `#cgo LDFLAGS` in `examples/go/main.go` to the installed library directory and platform linker flags. The example includes a Linux-style rpath for local development; macOS/Windows toolchains may need different flags.

Memory boundary rules:

- `C.CBytes` creates a C-owned copy for a Go byte slice. Free it with `C.free` after the synchronous call returns.
- Never pass a pointer into a Go slice to a function that may retain it beyond the call. The current API copies edit/create input bytes before returning, but the conservative rule remains safer for future APIs.
- Do not convert arbitrary bytes to a C string to discover its length; NUL is valid document content. Use the explicit size returned by the API.
- A C `uint64_t` maps to `C.uint64_t` / Go `uint64`; a document handle is not a pointer and should not be cast to a pointer.

## 4. JavaScript (`node` + Koffi)

Requirements: Node.js with native-addon support and npm access or a cached Koffi package. The repository's `examples/node/package.json` declares Koffi.

```sh
make ffi-shared
cd examples/node
npm install
npm start
```

Expected stdout is `Hello CDIN`; the version goes to stderr. Set `CDIN_FFI_LIB` to use a bridge installed outside the repository. See the [Koffi quick start](https://koffi.dev/start) and [pointer documentation](https://koffi.dev/pointers) for Koffi's version-specific FFI and pointer rules.

The JavaScript example intentionally uses `Buffer` and explicit lengths. This is the correct pattern for arbitrary document data; `Buffer.from(text, 'utf8')` is suitable only when the application intentionally starts from Unicode text. For an existing file, read the file as bytes instead of decoding and re-encoding it before sending it to the kernel. The repository also contains a TypeScript version, `quickstart.ts`; run it with `npm run start:ts` after `npm install` to install the optional TypeScript development dependencies.

Koffi maps C `uint64_t` results to `BigInt` in the example. Keep these values as `bigint` (`0n`, `6n`, etc.) rather than converting offsets or handles to JavaScript `Number`, which cannot exactly represent every integer above `2^53 - 1`.

## 5. TypeScript

Koffi operates at runtime; TypeScript adds compile-time types around those calls. A small wrapper should make status handling and `bigint` usage explicit rather than spreading raw FFI prototypes throughout the application. For example:

```ts
export type CdinHandle = bigint;
export type ByteOffset = bigint;

export function assertCdinOk(status: number, message: string): void {
  if (status !== 0) throw new Error(`${message}: ${status}`);
}

export function asBytes(input: Uint8Array): Buffer {
  // Buffer.from copies the bytes and preserves embedded NUL values.
  return Buffer.from(input);
}
```

The complete TypeScript example is `examples/node/quickstart.ts`; it mirrors the Koffi declarations and lifetime handling in `quickstart.mjs`. It is a standalone example, not a packaged SDK. In your own project, add `@types/node` and a TypeScript build pipeline as appropriate, compile the wrapper, then run it with a supported Node.js runtime.

## 6. Calling the core ABI directly

Use the direct ABI when you need the complete document/search/transaction/snapshot/notification surface or want to avoid an extra bridge layer. The tradeoff is that you must mirror the C ABI exactly:

1. Generate or write bindings against the **same** `include/cdin/kernel.h` shipped with the shared library.
2. Match C widths exactly (`uint32_t`, `uint64_t`, `size_t`, pointers, callback calling convention, and structure alignment).
3. Zero-initialize each request/config/result struct, set `struct_size = sizeof(struct)`, and set reserved flags to zero.
4. Supply a conforming allocator (`alloc`, `realloc`, `free`) to `cdin_kernel_create`. Allocation sizes passed to `realloc` and `free` must be honored as documented.
5. Check every `cdin_status`; do not inspect outputs unless the return status permits it.
6. Query `cdin_abi_version` and `cdin_features` at startup, but remember that ABI `0.x` is not promised binary-compatible across arbitrary revisions.

A binding should expose its own safe language types instead of passing raw kernel pointers across application/plugin/network boundaries. Handles belong to the kernel that issued them and remain valid only for the documented lifetime.

## 7. Bridge API surface

`examples/ffi/cdin_ffi.h` exposes only these convenience functions:

| Function | Purpose |
|---|---|
| `cdin_ffi_kernel_create` / `cdin_ffi_kernel_destroy` | Create/destroy a kernel with the example's `malloc` allocator. |
| `cdin_ffi_doc_create` | Create a document from bytes and a length. |
| `cdin_ffi_doc_replace` | Replace `[start, end)` against an expected document version; returns the new version. |
| `cdin_ffi_doc_length` | Get document length in bytes. |
| `cdin_ffi_doc_read` | Query/read the complete document. Null buffer performs a size query. |
| `cdin_ffi_doc_close` | Close a document. |
| `cdin_ffi_status_name` | Convert a status to its symbolic name. |

The bridge intentionally does not wrap every kernel feature. For batch edits, transactions, history blobs, snapshots, notifications, positions, and search, add bridge functions that call the corresponding public API; do not reimplement those features in the language binding. Whenever the header or core changes, rebuild and retest the bridge.

## 8. Common integration failures

| Symptom | Likely cause and next step |
|---|---|
| Library not found | Build `ffi-shared`, set `CDIN_FFI_LIB`, and check OS/CPU architecture match. |
| Crash or nonsensical lengths | Fix FFI argument/return types; ensure `uint64_t` stays 64-bit and pointers use pointer types. |
| `CDIN_VERSION_MISMATCH` | Header and runtime library have incompatible ABI majors; rebuild/package them together. |
| `CDIN_BUFFER_TOO_SMALL` | Resize from `out_len` and retry; do not assume the first query remains valid if another thread can mutate the document between calls. Use a snapshot for a stable read. |
| `CDIN_STALE_VERSION` | Read the current version and recompute byte offsets instead of blindly retrying the old edit. |
| Non-ASCII edits land in the wrong place | Offsets are bytes, not Unicode code points or grapheme clusters. Convert UI positions using the text-position APIs. |
| Invalid text changes on round trip | Avoid decoding/re-encoding raw document bytes outside the kernel. Read and write bytes unless normalization is an explicit application policy. |
| Go linker failure | Ensure `CGO_ENABLED=1`, install a C toolchain, and adjust the example's platform-specific `#cgo LDFLAGS`. |
