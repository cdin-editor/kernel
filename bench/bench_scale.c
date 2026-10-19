/* Scale benchmark for realistic large documents. Emits CSV: size, timings, throughput. */
#define _POSIX_C_SOURCE 200809L
#include "cdin/kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) { struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0.0; return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static void *b_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void *b_realloc(void *ctx, void *p, size_t old_n, size_t new_n) { (void)ctx; (void)old_n; if (!new_n) { free(p); return NULL; } return realloc(p, new_n); }
static void b_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
#define CHECK(call) do { cdin_status st_ = (call); if (st_ != CDIN_OK) { fprintf(stderr, #call " failed: status=%d\n", (int)st_); free(data); if (k) cdin_kernel_destroy(k); return 1; } } while (0)

int main(int argc, char **argv) {
    size_t mib = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 125;
    if (mib == 0 || mib > 1024) { fprintf(stderr, "size must be 1..1024 MiB\n"); return 2; }
    size_t n = mib * (size_t)1024 * 1024;
    uint8_t *data = malloc(n);
    if (!data) { fprintf(stderr, "cannot allocate input (%zu MiB)\n", mib); return 1; }
    /* Multi-line ASCII payload: exercises line indexing as well as byte operations. */
    static const char line[] = "cdin-kernel benchmark payload; safe editing; UTF-8 API; line data\n";
    for (size_t i = 0; i < n; ++i) data[i] = (uint8_t)line[i % (sizeof(line) - 1)];

    cdin_allocator a = { sizeof(cdin_allocator), b_alloc, b_realloc, b_free, NULL };
    cdin_kernel_config cfg; memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg; cfg.allocator = a;
    cfg.max_documents = 2; cfg.max_document_size = (uint64_t)n + 4096; cfg.max_undo_memory = 16u * 1024u * 1024u;
    cdin_kernel *k = NULL; double t0 = now_s();
    cdin_status st = cdin_kernel_create(&cfg, &k);
    double create_s = now_s() - t0;
    if (st != CDIN_OK) { fprintf(stderr, "kernel create failed: status=%d\n", (int)st); free(data); return 1; }
    cdin_doc d; t0 = now_s(); st = cdin_doc_create(k, data, n, &d); double load_s = now_s() - t0;
    if (st != CDIN_OK) { fprintf(stderr, "document create failed: status=%d\n", (int)st); cdin_kernel_destroy(k); free(data); return 1; }
    free(data); data = NULL;

    uint64_t lines = 0; t0 = now_s(); st = cdin_text_line_count(k, cdin_src_doc(d), &lines); double lines_s = now_s() - t0;
    if (st != CDIN_OK) { fprintf(stderr, "line count failed: status=%d\n", (int)st); cdin_doc_close(k,d); cdin_kernel_destroy(k); return 1; }
    cdin_search_request q; cdin_search_result r; uint64_t range[2];
    memset(&q,0,sizeof q); q.struct_size=sizeof q; q.src=cdin_src_doc(d); q.expected_version=CDIN_ANY_VERSION;
    q.pattern=(const uint8_t *)"needle-that-is-not-present"; q.pattern_len=strlen((const char *)q.pattern); q.bound=UINT64_MAX; q.limit=1; q.ranges=range; q.ranges_capacity=1;
    memset(&r,0,sizeof r); r.struct_size=sizeof r; t0=now_s(); st=cdin_search(k,&q,&r); double search_s=now_s()-t0;
    if (st != CDIN_OK) { fprintf(stderr,"search failed: status=%d\n",(int)st); cdin_doc_close(k,d); cdin_kernel_destroy(k); return 1; }
    cdin_snapshot snap; cdin_version version; t0=now_s(); st=cdin_snapshot_acquire(k,d,&version,&snap); double snapshot_s=now_s()-t0;
    if (st != CDIN_OK) { fprintf(stderr,"snapshot failed: status=%d\n",(int)st); cdin_doc_close(k,d); cdin_kernel_destroy(k); return 1; }
    cdin_snapshot_release(k,snap); cdin_doc_close(k,d); cdin_kernel_destroy(k);
    printf("%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.2f,%llu\n", mib, create_s, load_s, lines_s, search_s, snapshot_s,
      load_s > 0 ? ((double)n / (1024.0*1024.0)) / load_s : 0.0, (unsigned long long)lines);
    return 0;
}
