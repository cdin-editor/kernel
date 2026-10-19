#include "cdin/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *default_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void *default_realloc(void *ctx, void *p, size_t old_n, size_t new_n)
{
    (void)ctx;
    (void)old_n; /* realloc() does not need the old size; CDIN still passes it. */
    return realloc(p, new_n);
}

static void default_free(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n; /* The allocator contract requires the size; libc free() ignores it. */
    free(p);
}

#define CHECK(call) do { \
    cdin_status check_status = (call); \
    if (check_status != CDIN_OK) { \
        fprintf(stderr, "%s failed: %s (%d)\n", #call, \
                cdin_status_name(check_status), (int)check_status); \
        goto cleanup; \
    } \
} while (0)

int main(void)
{
    cdin_kernel_config cfg = {0};
    cdin_kernel *kernel = NULL;
    cdin_doc doc = CDIN_INVALID_DOC;
    cdin_status st = CDIN_OK;
    const uint8_t initial[] = "Hello world\n";
    const uint8_t replacement[] = "CDIN";
    cdin_replace_request req = {0};
    cdin_edit_result result = {0};
    uint64_t len = 0;
    size_t got = 0;
    uint8_t *buffer = NULL;
    int exit_code = 1;

    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator.struct_size = (uint32_t)sizeof cfg.allocator;
    cfg.allocator.alloc = default_alloc;
    cfg.allocator.realloc = default_realloc;
    cfg.allocator.free = default_free;
    cfg.lock.struct_size = (uint32_t)sizeof cfg.lock;
    cfg.log.struct_size = (uint32_t)sizeof cfg.log;

    CHECK(cdin_kernel_create(&cfg, &kernel));
    CHECK(cdin_doc_create(kernel, initial, sizeof initial - 1, &doc));

    req.struct_size = (uint32_t)sizeof req;
    req.expected_version = 0; /* New documents start at version 0. */
    req.start = 6;             /* Byte range [6, 11): "world". */
    req.end = 11;
    req.bytes = replacement;
    req.len = sizeof replacement - 1;
    result.struct_size = (uint32_t)sizeof result;
    CHECK(cdin_doc_replace(kernel, doc, &req, &result));

    CHECK(cdin_text_len(kernel, cdin_src_doc(doc), &len));
    if (len > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "Document is too large for this process.\n");
        goto cleanup;
    }
    buffer = (uint8_t *)malloc((size_t)(len ? len : 1));
    if (!buffer) {
        fprintf(stderr, "Out of memory.\n");
        goto cleanup;
    }
    CHECK(cdin_text_read(kernel, cdin_src_doc(doc), 0, len,
                         buffer, (size_t)len, &got, NULL));
    if (fwrite(buffer, 1, got, stdout) != got) {
        fprintf(stderr, "Failed to write output.\n");
        goto cleanup;
    }
    fprintf(stderr, "document version: %llu\n", (unsigned long long)result.new_version);
    exit_code = 0;

cleanup:
    free(buffer);
    if (kernel && doc != CDIN_INVALID_DOC) {
        st = cdin_doc_close(kernel, doc);
        if (st != CDIN_OK && exit_code == 0) exit_code = 1;
    }
    if (kernel) {
        st = cdin_kernel_destroy(kernel);
        if (st != CDIN_OK && exit_code == 0) exit_code = 1;
    }
    return exit_code;
}
