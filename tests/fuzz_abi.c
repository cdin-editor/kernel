/*
 * Fuzz target: public API inputs.
 * Feed random bytes as document content and random parameters to API calls.
 * Run under ASan/UBSan.
 */
#include "cdin/kernel.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *fuzz_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void *fuzz_realloc(void *ctx, void *p, size_t old_n, size_t new_n) {
    (void)ctx; (void)old_n;
    if (new_n == 0) { free(p); return NULL; }
    return realloc(p, new_n);
}
static void fuzz_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16) return 0;
    cdin_allocator alloc = { sizeof(cdin_allocator), fuzz_alloc, fuzz_realloc, fuzz_free, NULL };
    cdin_kernel_config cfg = { sizeof(cdin_kernel_config), alloc, 64, 1024 };
    cdin_kernel *k = NULL;
    if (cdin_kernel_create(&cfg, &k) != CDIN_OK) return 0;

    /* Use first bytes as document content */
    cdin_doc d;
    cdin_doc_create(k, data, size > 256 ? 256 : size, &d);

    /* Random operations based on input bytes */
    for (size_t i = 0; i < 8 && i + 4 < size; i++) {
        uint8_t op = data[i] % 6;
        switch (op) {
        case 0: cdin_doc_version(k, d, &(cdin_version){0}); break;
        case 1: cdin_doc_len(k, d, &(uint64_t){0}); break;
        case 2: {
            cdin_replace_request req;
            req.struct_size = sizeof(req);
            req.start = 0;
            req.end = 1;
            req.bytes = data;
            req.len = 1;
            req.expected_version = CDIN_ANY_VERSION;
            req.coalesce_key = 0;
            cdin_doc_replace(k, d, &req, &(cdin_edit_result){0});
            break;
        }
        case 3: cdin_doc_undo(k, d, CDIN_ANY_VERSION, &(cdin_edit_result){0}); break;
        case 4: cdin_doc_redo(k, d, CDIN_ANY_VERSION, &(cdin_edit_result){0}); break;
        case 5: cdin_doc_close(k, d); break;
        }
    }
    cdin_kernel_destroy(k);
    return 0;
}
