/**
 * @file cdin_ffi.c
 * @brief Implementation of the small bridge used by the language examples.
 */
#include "cdin_ffi.h"

#include <stdlib.h>

static void *bridge_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void *bridge_realloc(void *ctx, void *p, size_t old_n, size_t new_n)
{
    (void)ctx;
    (void)old_n;
    return realloc(p, new_n);
}

static void bridge_free(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n;
    free(p);
}

cdin_status cdin_ffi_kernel_create(cdin_kernel **out)
{
    cdin_kernel_config cfg = {0};
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator.struct_size = (uint32_t)sizeof cfg.allocator;
    cfg.allocator.alloc = bridge_alloc;
    cfg.allocator.realloc = bridge_realloc;
    cfg.allocator.free = bridge_free;
    cfg.lock.struct_size = (uint32_t)sizeof cfg.lock;
    cfg.log.struct_size = (uint32_t)sizeof cfg.log;
    return cdin_kernel_create(&cfg, out);
}

cdin_status cdin_ffi_kernel_destroy(cdin_kernel *kernel)
{
    return cdin_kernel_destroy(kernel);
}

cdin_status cdin_ffi_doc_create(cdin_kernel *kernel, const uint8_t *bytes,
                                size_t len, cdin_doc *out_doc)
{
    return cdin_doc_create(kernel, bytes, len, out_doc);
}

cdin_status cdin_ffi_doc_replace(cdin_kernel *kernel, cdin_doc doc,
                                 cdin_version expected_version,
                                 uint64_t start, uint64_t end,
                                 const uint8_t *bytes, size_t len,
                                 cdin_version *out_new_version)
{
    cdin_replace_request req = {0};
    cdin_edit_result result = {0};
    cdin_status st;
    if (!out_new_version) return CDIN_INVALID_ARGUMENT;
    req.struct_size = (uint32_t)sizeof req;
    req.expected_version = expected_version;
    req.start = start;
    req.end = end;
    req.bytes = bytes;
    req.len = len;
    result.struct_size = (uint32_t)sizeof result;
    st = cdin_doc_replace(kernel, doc, &req, &result);
    if (st == CDIN_OK) *out_new_version = result.new_version;
    return st;
}

cdin_status cdin_ffi_doc_length(cdin_kernel *kernel, cdin_doc doc,
                                uint64_t *out_len)
{
    return cdin_text_len(kernel, cdin_src_doc(doc), out_len);
}

cdin_status cdin_ffi_doc_read(cdin_kernel *kernel, cdin_doc doc,
                              uint8_t *buf, size_t capacity, size_t *out_len)
{
    uint64_t len = 0;
    cdin_status st;
    if (!out_len) return CDIN_INVALID_ARGUMENT;
    st = cdin_text_len(kernel, cdin_src_doc(doc), &len);
    if (st != CDIN_OK) return st;
    if (len > (uint64_t)SIZE_MAX) return CDIN_LIMIT_EXCEEDED;
    return cdin_text_read(kernel, cdin_src_doc(doc), 0, len,
                          buf, capacity, out_len, NULL);
}

cdin_status cdin_ffi_doc_close(cdin_kernel *kernel, cdin_doc doc)
{
    return cdin_doc_close(kernel, doc);
}

const char *cdin_ffi_status_name(cdin_status status)
{
    return cdin_status_name(status);
}
