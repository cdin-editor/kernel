/* Fuzz target: line, raw-region and Unicode position classifiers. */
#include <cdin/kernel.h>
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
    if (size < 1) return 0;
    cdin_kernel_config cfg;
    cdin_allocator alloc;
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;
    memset(&cfg, 0, sizeof cfg); memset(&alloc, 0, sizeof alloc);
    alloc.struct_size = (uint32_t)sizeof alloc;
    alloc.alloc = fuzz_alloc; alloc.realloc = fuzz_realloc; alloc.free = fuzz_free;
    cfg.struct_size = (uint32_t)sizeof cfg; cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator = alloc;
    if (cdin_kernel_create(&cfg, &k) != CDIN_OK) return 0;
    if (cdin_doc_create(k, data, size, &d) != CDIN_OK) { cdin_kernel_destroy(k); return 0; }

    cdin_source src = cdin_src_doc(d);
    uint64_t lc = 0, len = 0;
    if (cdin_text_line_count(k, src, &lc) == CDIN_OK) {
        for (uint64_t i = 0; i < lc && i < 64; i++) {
            uint64_t start = 0, line = 0, col = 0;
            if (cdin_text_line_start(k, src, i, &start) == CDIN_OK)
                (void)cdin_text_line_from_offset(k, src, start, &line, &col);
        }
    }
    if (cdin_text_len(k, src, &len) == CDIN_OK) {
        for (uint64_t i = 0; i <= len; i++) {
            uint64_t cp = 0;
            uint32_t flags = 0;
            (void)cdin_text_byte_to_codepoint(k, src, i, &cp, &flags);
            (void)cdin_text_check_position(k, src, CDIN_ANY_VERSION, i);
        }
    }
    (void)cdin_doc_close(k, d);
    (void)cdin_kernel_destroy(k);
    return 0;
}
