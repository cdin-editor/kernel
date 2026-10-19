/*
 * Fuzz target: region and line classifiers.
 * Feed random bytes with CR and LF in every combination.
 * Run under ASan/UBSan.
 */
#include "cdin/kernel.h"
#include <stdint.h>
#include <stdlib.h>

static void *fuzz_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void *fuzz_realloc(void *ctx, void *p, size_t old_n, size_t new_n) {
    (void)ctx; (void)old_n;
    if (new_n == 0) { free(p); return NULL; }
    return realloc(p, new_n);
}
static void fuzz_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 8) return 0;
    cdin_allocator alloc = { sizeof(cdin_allocator), fuzz_alloc, fuzz_realloc, fuzz_free, NULL };
    cdin_kernel_config cfg = { sizeof(cdin_kernel_config), alloc, 64, 1024 };
    cdin_kernel *k = NULL;
    if (cdin_kernel_create(&cfg, &k) != CDIN_OK) return 0;

    cdin_doc d;
    cdin_doc_create(k, data, size, &d);

    uint64_t lc = 0;
    cdin_doc_line_count(k, d, &lc);
    for (uint64_t i = 0; i < lc && i < 64; i++) {
        uint64_t start = 0;
        cdin_doc_line_start(k, d, i, &start);
        uint64_t line = 0, col = 0;
        cdin_doc_line_from_offset(k, d, start, &line, &col);
    }
    for (size_t i = 0; i < size; i++) {
        uint64_t cp = 0;
        cdin_doc_byte_to_codepoint(k, d, i, &cp);
        int valid = 0;
        cdin_doc_check_position(k, d, i, &valid);
    }
    cdin_doc_close(k, d);
    cdin_kernel_destroy(k);
    return 0;
}
