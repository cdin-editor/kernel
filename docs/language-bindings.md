# Language bindings

`cdin-kernel` is a C11 library with a frozen C ABI. Any runtime that can call
a C shared library can use it. This document first states the rules that
apply to every binding, then walks through Python, Go, JavaScript/TypeScript,
C++, and the checklist for other languages.

Two integration routes exist:

1. **Direct C ABI.** Declare each function and struct exactly as the header
   defines them. Required when you need features the bridge does not wrap:
   transactions, snapshots, notifications, search, undo, text helpers.
2. **The example bridge (`cdin_ffi`).** A small C shim in
   `examples/ffi/` that installs a standard allocator and exposes a few
   scalar operations. It exists so a binding can be written in minutes; it
   is sample code, not part of the stable ABI. Extend it freely in your own
   tree.

Route 1 is the production answer; route 2 is the tutorial answer. The
concepts below are identical for both.

## 1. Rules every binding must follow

| Rule | Why |
|---|---|
| Pass bytes as pointer + length. Never rely on NUL termination. | Documents are byte sequences; NUL inside is normal. |
| Handles are opaque `uint64`/`BigInt`. Treat them as tokens. | Types are separate namespaces; a doc handle in a `cdin_notif` slot fails with `CDIN_INVALID_HANDLE` (or, worse, could address another object's slot numerically). Keep them distinct in your API layer. |
| Set `struct_size` on every request/output struct you pass. | The struct-size protocol rejects unknown shapes (see the ABI reference). |
| Statuses: `CDIN_OK == 0`. Check every call. | One missed `STALE_VERSION` becomes silent data loss. |
| Zero your request structs before setting fields. | Reserved fields must be 0. |
| Keep callbacks and their contexts alive for the kernel's lifetime. | The kernel stores pointers, not copies, of allocator/lock/wakeup callbacks. |
| Callbacks must not re-enter the kernel. | Allocator runs under the caller's lock; re-entry deadlocks non-recursive locks. |
| One thread at a time, or a lock vtable. | See the threading section of the user guide. For a runtime with its own scheduler (JS), call the kernel from a single dedicated thread or provide a mutex via native code. |
| Buffers you pass in are yours; the library copies out-of everything it needs. | Free caller memory as soon as a call returns; the kernel owns its own storage. |
| Manage `close`/`destroy` order explicitly in destructors or finalizers. | The kernel does not track who calls it; there is no reanimation of closed handles. |

## 2. The compiled artifacts

| File | Purpose |
|---|---|
| `build/libcdin_kernel.a` / `.so` / `.dylib` | The kernel. This is the target of any direct-ABI binding. |
| `build/cdin_kernel.dll` | Same, Windows. |
| `build/libcdin_ffi.so` / `.dylib` / `cdin_ffi.dll` | Example bridge; built by `make ffi-shared`. |

Package managers usually want the **kernel** library itself. The bridge is a
single small C file you can vendor into your binding if you prefer.

## 3. Python (ctypes)

The repository ships `examples/python/quickstart.py`, which loads
`cdin_ffi` and exercises the bridge. The same structure works when loading
`libcdin_kernel.so` directly.

```python
import ctypes, os, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
lib = ctypes.CDLL(str(ROOT / "build" / "libcdin_kernel.so"))  # dylib/dll name by platform
CDIN_OK = 0

kernel_create = lib.cdin_ffi_kernel_create                 # bridge variant
kernel_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
kernel_create.restype = ctypes.c_int

status_name = lib.cdin_ffi_status_name
status_name.argtypes = [ctypes.c_int]
status_name.restype = ctypes.c_char_p
```

For the *direct* ABI, there are two obstacles that decide whether you bind
the kernel itself or write a shim:

1. **Allocator callbacks.** `cdin_kernel_create` requires a `cdin_allocator`
   whose three C callbacks never go away. In ctypes you can construct the
   struct with `CFUNCTYPE` fields backed by `libc` calls
   (`ctypes.CDLL(None)`), but the callbacks must call `malloc`/`realloc`
   through pointers you keep referenced for the process lifetime, and they
   must not raise. This is mechanical but easy to get subtly wrong.
2. **`cdin_source` is passed by value.** FFI layers differ in
   struct-by-value support. ctypes can wrap it in a small class but the
   concrete calling convention is platform-specific; a 10-line C shim that
   takes two `uint64` arguments is the robust answer.

Recommendation for a production Python binding: vendor a ~30-line C shim
(modeled on `examples/ffi/cdin_ffi.c`) that exposes exactly the operations
your editor needs, then bind only scalars in ctypes. This keeps the ABI
interesting parts in C where they compile without guesswork.

## 4. Go (cgo)

`examples/go/main.go` is a complete program. The pattern, condensed:

```go
/*
#cgo CFLAGS: -I${SRCDIR}/../ffi -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -lcdin_ffi -Wl,-rpath,${SRCDIR}/../../build
#include <stdlib.h>
#include "cdin_ffi.h"
*/
import "C"

func check(status C.cdin_status) error {
    if status == C.CDIN_OK {
        return nil
    }
    return fmt.Errorf("cdin: %s", C.GoString(C.cdin_ffi_status_name(status)))
}
```

Notes:

- `C.CBytes` / `C.GoStringN` are the boundary: copy on the way in and on the
  way out, then `C.free` what you handed to C. The kernel copies again
  internally, so the Go byte slice may be reused immediately after a call.
- Call sites are synchronous in the C sense; the Go scheduler expects cgo
  calls that do not block indefinitely. All kernel calls are bounded and
  non-blocking except a search that you gave a huge step budget to.
- The `-rpath` in `LDFLAGS` is for the example; a packaged binding should
  link with `-L` and a runtime path policy you control.
- Never hold `C.CBytes`-allocated memory across awaits; cgo pins it.

## 5. JavaScript / TypeScript (koffi)

`examples/node/quickstart.mjs` (plain JS) and `quickstart.ts` (types) load
`libcdin_ffi` with [`koffi`](https://koffi.dev). The kernel's public ABI is
also directly usable because every entry point is `CDECL` free of inline
structs by value except `cdin_source` in read helpers; the bridge avoids even
that.

```js
import koffi from 'koffi';
const lib = koffi.load('build/libcdin_ffi.so');   // dylib/dll by platform

const docCreate = lib.func('int cdin_ffi_doc_create(void *kernel, const uint8_t *bytes, size_t len, _Out_ uint64_t *out_doc)');
const docReplace = lib.func('int cdin_ffi_doc_replace(void *kernel, uint64_t doc, uint64_t expected_version, uint64_t start, uint64_t end, const uint8_t *bytes, size_t len, _Out_ uint64_t *out_new_version)');
const docRead = lib.func('int cdin_ffi_doc_read(void *kernel, uint64_t doc, _Out_ uint8_t *buf, size_t capacity, _Out_ size_t *out_len)');
```

- `Buffer` instances map to `const uint8_t *`; pass `Buffer.byteLength` for
  length. The kernel copies the bytes before returning, so a `Buffer` can be
  GC'd afterwards.
- 64-bit handles are `BigInt` (`0n` check against `CDIN_OK == 0n`).
- Arrays passed for `_Out_` must be exactly `[initialValue]`; koffi writes
  back through the reference.
- Buffers passed for `_Out_ uint8_t *` are allocated by you; the read
  pattern is: call with `null` and capacity `0` to learn the size, allocate
  exactly, call again.

## 6. C and C++

The header works with a plain C compiler and as C++ (it wraps everything in
`extern "C"`). A C++ application should:

- include `<cdin/kernel.h>`,
- wrap the kernel handle in an RAII type whose destructor calls
  `cdin_kernel_destroy`,
- keep the allocator/lock objects and their contexts alive at least as long
  as the kernel.

No exceptions and no templates cross the ABI; everything stays POD.

## 7. Other languages (checklist)

| Language | Technique | Notes |
|---|---|---|
| Rust | `bindgen` on `include/cdin/kernel.h` (or `cdin_ffi.h`) | Opaque kernel pointer as `ExternalApi`; keep allocator callbacks `'static`. |
| Zig | `@cImport` of `kernel.h` | Direct ABI works; watch for `size_t` vs `usize` conversions. |
| C#/.NET | `DllImport` with Blittable structs | The `cdin_source`-by-value helpers need a C shim; everything else binds directly. `[SkipLocalsInit]`-style zeroing is unnecessary: explicit `Marshal.StructureToPtr` paths with `struct_size` set are the safe pattern. |
| Java (Panama) | `downcall` with explicit layouts | Wrap the allocator callbacks into `upcall stubs` whose lifetime equals the kernel's. |

General rule: the safest minimal version of a binding for a language is an
explicit `manifest` of the handful of entry points you need, not an
auto-complete mapping of the header. That is what the bridge examples in
`examples/` are.

## 8. Testing a binding

Smoke-test each new or changed binding against the same sequence as the
repository quickstart:

1. create kernel,
2. `doc_create` with `hello`,
3. replace to `Hello, CDIN` at `[0, 5)`,
4. read the full document back,
5. close the document, destroy the kernel.

Expected results: the document version is `1`, the length is `11`, and the
read-back content is exactly `Hello, CDIN`. If a binding loses or reorders
bytes at any boundary, this sequence catches it.
