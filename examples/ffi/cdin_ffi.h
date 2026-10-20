/**
 * @file cdin_ffi.h
 * @brief Small optional bridge used by the language-binding examples.
 *
 * This bridge is sample code and is not part of the stable public kernel ABI.
 * It supplies default allocation callbacks and exposes a few common operations
 * with scalar arguments that are easy to describe in FFI declarations. Rebuild
 * the bridge whenever the core library changes.
 */
#ifndef CDIN_FFI_EXAMPLE_H
#define CDIN_FFI_EXAMPLE_H

#include <cdin/kernel.h>

#if defined(_WIN32) && defined(CDIN_KERNEL_BUILD_DLL)
#  define CDIN_FFI_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define CDIN_FFI_API __attribute__((visibility("default")))
#else
#  define CDIN_FFI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Create a kernel using the bridge's standard C allocator. */
CDIN_FFI_API cdin_status cdin_ffi_kernel_create(cdin_kernel **out);

/** Destroy the kernel and release all handles it owns. */
CDIN_FFI_API cdin_status cdin_ffi_kernel_destroy(cdin_kernel *kernel);

/** Create a document by copying the supplied byte sequence. */
CDIN_FFI_API cdin_status cdin_ffi_doc_create(cdin_kernel *kernel,
                                              const uint8_t *bytes, size_t len,
                                              cdin_doc *out_doc);

/** Replace a byte range if the document is at `expected_version`. */
CDIN_FFI_API cdin_status cdin_ffi_doc_replace(cdin_kernel *kernel, cdin_doc doc,
                                               cdin_version expected_version,
                                               uint64_t start, uint64_t end,
                                               const uint8_t *bytes, size_t len,
                                               cdin_version *out_new_version);

/** Return the document length in bytes. */
CDIN_FFI_API cdin_status cdin_ffi_doc_length(cdin_kernel *kernel, cdin_doc doc,
                                              uint64_t *out_len);

/** Copy the complete document; a NULL buffer queries its required size. */
CDIN_FFI_API cdin_status cdin_ffi_doc_read(cdin_kernel *kernel, cdin_doc doc,
                                            uint8_t *buf, size_t capacity,
                                            size_t *out_len);

/** Close a document before destroying its kernel. */
CDIN_FFI_API cdin_status cdin_ffi_doc_close(cdin_kernel *kernel, cdin_doc doc);

/** Return the static, NUL-terminated name for a status code. */
CDIN_FFI_API const char *cdin_ffi_status_name(cdin_status status);

#ifdef __cplusplus
}
#endif
#endif /* CDIN_FFI_EXAMPLE_H */
