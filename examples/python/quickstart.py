"""Python ctypes example using the optional FFI bridge built by `make ffi-shared`."""
from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if os.environ.get("CDIN_FFI_LIB"):
    LIBRARY = os.environ["CDIN_FFI_LIB"]
elif os.name == "nt":
    LIBRARY = str(ROOT / "build" / "cdin_ffi.dll")
elif sys.platform == "darwin":
    LIBRARY = str(ROOT / "build" / "libcdin_ffi.dylib")
else:
    LIBRARY = str(ROOT / "build" / "libcdin_ffi.so")

lib = ctypes.CDLL(LIBRARY)
status_name = lib.cdin_ffi_status_name
status_name.argtypes = [ctypes.c_int]
status_name.restype = ctypes.c_char_p

kernel_create = lib.cdin_ffi_kernel_create
kernel_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
kernel_create.restype = ctypes.c_int
kernel_destroy = lib.cdin_ffi_kernel_destroy
kernel_destroy.argtypes = [ctypes.c_void_p]
kernel_destroy.restype = ctypes.c_int

doc_create = lib.cdin_ffi_doc_create
doc_create.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                       ctypes.POINTER(ctypes.c_uint64)]
doc_create.restype = ctypes.c_int
doc_replace = lib.cdin_ffi_doc_replace
doc_replace.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                        ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8),
                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64)]
doc_replace.restype = ctypes.c_int
doc_read = lib.cdin_ffi_doc_read
doc_read.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8),
                     ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
doc_read.restype = ctypes.c_int
doc_close = lib.cdin_ffi_doc_close
doc_close.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
doc_close.restype = ctypes.c_int


def check(status: int) -> None:
    if status != 0:
        name = status_name(status).decode("ascii", errors="replace")
        raise RuntimeError(f"cdin call failed: {name} ({status})")


def byte_array(data: bytes):
    if not data:
        return None
    return (ctypes.c_uint8 * len(data)).from_buffer_copy(data)


def main() -> None:
    kernel = ctypes.c_void_p()
    doc = ctypes.c_uint64(0)
    try:
        check(kernel_create(ctypes.byref(kernel)))
        original = b"Hello world\n"
        check(doc_create(kernel, byte_array(original), len(original), ctypes.byref(doc)))

        replacement = b"CDIN"
        new_version = ctypes.c_uint64(0)
        check(doc_replace(kernel, doc.value, 0, 6, 11, byte_array(replacement),
                          len(replacement), ctypes.byref(new_version)))

        required = ctypes.c_size_t(0)
        check(doc_read(kernel, doc.value, None, 0, ctypes.byref(required)))
        result = (ctypes.c_uint8 * required.value)()
        actual = ctypes.c_size_t(0)
        check(doc_read(kernel, doc.value, result, required.value, ctypes.byref(actual)))
        sys.stdout.buffer.write(bytes(result[:actual.value]))
        print(f"document version: {new_version.value}", file=sys.stderr)
    finally:
        if kernel.value and doc.value:
            check(doc_close(kernel, doc.value))
        if kernel.value:
            check(kernel_destroy(kernel))


if __name__ == "__main__":
    main()
