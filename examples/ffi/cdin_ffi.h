#ifndef CDIN_FFI_EXAMPLE_H
#define CDIN_FFI_EXAMPLE_H

#include "cdin/kernel.h"

/*
 * OPTIONAL EXAMPLE BRIDGE, NOT PART OF THE STABLE cdin-kernel ABI.
 * It presents a deliberately small ABI for languages whose FFI support makes
 * nested C structs and allocator callbacks inconvenient. Rebuild this bridge
 * whenever you update cdin-kernel.
 */
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

CDIN_FFI_API cdin_status cdin_ffi_kernel_create(cdin_kernel **out);
CDIN_FFI_API cdin_status cdin_ffi_kernel_destroy(cdin_kernel *kernel);
CDIN_FFI_API cdin_status cdin_ffi_doc_create(cdin_kernel *kernel,
                                              const uint8_t *bytes, size_t len,
                                              cdin_doc *out_doc);
CDIN_FFI_API cdin_status cdin_ffi_doc_replace(cdin_kernel *kernel, cdin_doc doc,
                                               cdin_version expected_version,
                                               uint64_t start, uint64_t end,
                                               const uint8_t *bytes, size_t len,
                                               cdin_version *out_new_version);
CDIN_FFI_API cdin_status cdin_ffi_doc_length(cdin_kernel *kernel, cdin_doc doc,
                                              uint64_t *out_len);
/* Read the entire document; buf == NULL is a byte-count query. */
CDIN_FFI_API cdin_status cdin_ffi_doc_read(cdin_kernel *kernel, cdin_doc doc,
                                            uint8_t *buf, size_t capacity,
                                            size_t *out_len);
CDIN_FFI_API cdin_status cdin_ffi_doc_close(cdin_kernel *kernel, cdin_doc doc);
CDIN_FFI_API const char *cdin_ffi_status_name(cdin_status status);

#ifdef __cplusplus
}
#endif
#endif
