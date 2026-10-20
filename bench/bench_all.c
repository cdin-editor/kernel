/*
 * Benchmark: all operations.
 * Usage: bench_all <document_size_mb>
 */
#include "cdin/kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *bench_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void *bench_realloc(void *ctx, void *p, size_t old_n, size_t new_n) {
    (void)ctx; (void)old_n;
    if (new_n == 0) { free(p); return NULL; }
    return realloc(p, new_n);
}
static void bench_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

int main(int argc, char **argv) {
    size_t size_mb = (argc > 1) ? (size_t)atoi(argv[1]) : 1;
    size_t size = size_mb * 1024 * 1024;
    cdin_allocator alloc = { sizeof(cdin_allocator), bench_alloc, bench_realloc, bench_free, NULL };
    cdin_kernel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator = alloc;
    cfg.max_documents = 64;
    cfg.max_document_size = size + 1024;
    cfg.max_undo_memory = 64u * 1024u * 1024u;
    cfg.max_blob_size = 65536;

    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);

    clock_t t0 = clock();
    cdin_kernel *k = NULL;
    if (cdin_kernel_create(&cfg, &k) != CDIN_OK) { printf("create failed\n"); return 1; }
    clock_t t1 = clock();

    uint8_t *data = malloc(size);
    memset(data, 'a', size);

    cdin_doc d;
    cdin_doc_create(k, data, size, &d);
    clock_t t2 = clock();

    uint64_t lc = 0;
    cdin_doc_line_count(k, d, &lc);
    clock_t t3 = clock();

    uint64_t cp = 0;
    cdin_doc_byte_to_codepoint(k, d, size / 2, &cp);
    clock_t t4 = clock();

    for (uint64_t i = 0; i < 1000 && i < (uint64_t)size; i++) {
        cdin_replace_request req;
        req.struct_size = sizeof(req);
        req.start = i;
        req.end = i + 1;
        req.bytes = (uint8_t *)"b";
        req.len = 1;
        req.expected_version = CDIN_ANY_VERSION;
        req.coalesce_key = 0;
        cdin_doc_replace(k, d, &req, &er);
    }
    clock_t t5 = clock();

    for (int i = 0; i < 100; i++) {
        cdin_doc_undo(k, d, CDIN_ANY_VERSION, &er);
    }
    clock_t t6 = clock();

    cdin_search_result sr;
    memset(&sr, 0, sizeof(sr));
    sr.struct_size = sizeof(sr);
    cdin_search(k, d, (uint8_t *)"a", 1, CDIN_SEARCH_LITERAL, CDIN_SEARCH_FORWARD, 0, 1, 100, NULL, &sr);
    clock_t t7 = clock();

    cdin_snapshot s;
    cdin_version sv;
    cdin_snapshot_acquire(k, d, &sv, &s);
    clock_t t8 = clock();
    cdin_snapshot_release(k, s);

    cdin_doc_close(k, d);
    cdin_kernel_destroy(k);
    free(data);

    double cpus = (double)CLOCKS_PER_SEC;
    printf("size=%zu MB\n", size_mb);
    printf("kernel_create: %.3f s\n", (double)(t1 - t0) / cpus);
    printf("load_bytes: %.3f s\n", (double)(t2 - t1) / cpus);
    printf("line_count: %.3f s\n", (double)(t3 - t2) / cpus);
    printf("byte_to_cp: %.3f s\n", (double)(t4 - t3) / cpus);
    printf("1000_edits: %.3f s\n", (double)(t5 - t4) / cpus);
    printf("100_undos: %.3f s\n", (double)(t6 - t5) / cpus);
    printf("literal_search: %.3f s\n", (double)(t7 - t6) / cpus);
    printf("snapshot: %.3f s\n", (double)(t8 - t7) / cpus);
    return 0;
}
