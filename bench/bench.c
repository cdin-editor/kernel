/* Micro-benchmarks. Numbers are indicative only (one machine, no pinning); they exist to expose
 * complexity problems (EDIT-014, SEARCH-013), not to certify speed. Run: make bench */
#define _POSIX_C_SOURCE 200809L
#include <cdin/kernel.h>
#include "support/strict_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static cdin_kernel *K;

static cdin_doc make_doc(size_t n, const char *unit)
{
    size_t ul = strlen(unit), i; uint8_t *b = malloc(n); cdin_doc d;
    for (i = 0; i < n; i++) b[i] = (uint8_t)unit[i % ul];
    if (cdin_doc_create(K, b, n, &d) != CDIN_OK) { fprintf(stderr, "create failed\n"); exit(1); }
    free(b);
    return d;
}
static void ins(cdin_doc d, uint64_t at, const char *s, uint64_t key)
{
    cdin_replace_request r; cdin_edit_result o;
    memset(&r, 0, sizeof r); r.struct_size = sizeof r; r.expected_version = CDIN_ANY_VERSION; r.start = r.end = at; r.bytes = (const uint8_t *)s; r.len = strlen(s); r.coalesce_key = key;
    memset(&o, 0, sizeof o); o.struct_size = sizeof o;
    if (cdin_doc_replace(K, d, &r, &o) != CDIN_OK) { fprintf(stderr, "replace failed\n"); exit(1); }
}

int main(void)
{
    strict_alloc al; cdin_kernel_config c; size_t sizes[3] = { 1 << 20, 16 << 20, 128 << 20 };
    strict_init(&al);
    memset(&c, 0, sizeof c); c.struct_size = sizeof c; c.abi_major = CDIN_KERNEL_ABI_MAJOR; c.allocator = strict_vtable(&al); c.max_undo_memory = (uint64_t)1 << 28;
    cdin_kernel_create(&c, &K);
    printf("%-34s %12s %12s\n", "operation", "size", "time");
    for (int si = 0; si < 3; si++) {
        size_t n = sizes[si]; cdin_doc d = make_doc(n, "hello world line\n"); uint64_t cnt, len; double t;
        int reps = 2000;
        t = now(); for (int i = 0; i < reps; i++) ins(d, n / 2, "x", 0); t = now() - t;
        printf("%-34s %10zuMB %9.1f us\n", "insert 1 byte in the middle", n >> 20, t / reps * 1e6);
        t = now(); for (int i = 0; i < reps; i++) ins(d, n, "x", 0); t = now() - t;
        printf("%-34s %10zuMB %9.1f us\n", "append 1 byte", n >> 20, t / reps * 1e6);
        t = now(); for (int i = 0; i < reps; i++) ins(d, 10, "x", 7); t = now() - t;
        printf("%-34s %10zuMB %9.1f us\n", "coalesced insert near start", n >> 20, t / reps * 1e6);
        { cdin_history_request h; cdin_edit_result o; memset(&h, 0, sizeof h); h.struct_size = sizeof h; h.expected_version = CDIN_ANY_VERSION; memset(&o, 0, sizeof o); o.struct_size = sizeof o;
          t = now(); for (int i = 0; i < 1000; i++) cdin_doc_undo(K, d, &h, &o); t = now() - t;
          printf("%-34s %10zuMB %9.1f us\n", "undo (middle edits)", n >> 20, t / 1000 * 1e6); }
        t = now(); cdin_text_line_count(K, cdin_src_doc(d), &cnt); t = now() - t;
        printf("%-34s %10zuMB %9.1f ms\n", "line index build (first query)", n >> 20, t * 1e3);
        ins(d, 0, "y", 0);
        t = now(); cdin_text_line_count(K, cdin_src_doc(d), &cnt); t = now() - t;
        printf("%-34s %10zuMB %9.1f ms\n", "line index rebuild after edit", n >> 20, t * 1e3);
        { cdin_snapshot s; cdin_version v; cdin_snapshot_acquire(K, d, &v, &s);
          t = now(); ins(d, 5, "z", 0); t = now() - t;
          printf("%-34s %10zuMB %9.1f ms\n", "first edit after snapshot (COW)", n >> 20, t * 1e3);
          t = now(); ins(d, 5, "z", 0); t = now() - t;
          printf("%-34s %10zuMB %9.1f us\n", "second edit after snapshot", n >> 20, t * 1e6);
          cdin_snapshot_release(K, s); }
        { uint64_t rg[2]; cdin_search_request q; cdin_search_result r;
          memset(&q, 0, sizeof q); q.struct_size = sizeof q; q.src = cdin_src_doc(d); q.expected_version = CDIN_ANY_VERSION; q.pattern = (const uint8_t *)"NOTFOUND"; q.pattern_len = 8;
          q.bound = UINT64_MAX; q.limit = 1; q.ranges = rg; q.ranges_capacity = 1; memset(&r, 0, sizeof r); r.struct_size = sizeof r;
          t = now(); cdin_search(K, &q, &r); t = now() - t;
          printf("%-34s %10zuMB %9.1f ms\n", "literal search, no match", n >> 20, t * 1e3);
          q.flags = CDIN_SEARCH_CASE_INSENSITIVE; memset(&r, 0, sizeof r); r.struct_size = sizeof r;
          t = now(); cdin_search(K, &q, &r); t = now() - t;
          printf("%-34s %10zuMB %9.1f ms\n", "case-insensitive search, no match", n >> 20, t * 1e3);
          q.flags = 0; q.mode = CDIN_SEARCH_BYTES; memset(&r, 0, sizeof r); r.struct_size = sizeof r;
          t = now(); cdin_search(K, &q, &r); t = now() - t;
          printf("%-34s %10zuMB %9.1f ms\n", "bytes search, no match", n >> 20, t * 1e3); }
        cdin_text_len(K, cdin_src_doc(d), &len);
        { uint64_t o2; t = now(); for (int i = 0; i < 100000; i++) cdin_text_grapheme_next(K, cdin_src_doc(d), (uint64_t)i * 7 % (len - 8), &o2); t = now() - t;
          printf("%-34s %10zuMB %9.2f us\n", "grapheme_next", n >> 20, t / 100000 * 1e6); }
        cdin_doc_close(K, d);
    }
    cdin_kernel_destroy(K);
    return strict_check(&al, "bench");
}
