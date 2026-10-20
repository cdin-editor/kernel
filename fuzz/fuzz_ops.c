/* libFuzzer target: the input is a program of API calls (KERNEL-009: no input may crash, leak,
 * violate the allocator contract or corrupt state). Arguments are derived from the bytes, so the
 * fuzzer freely produces invalid positions, handles, versions, flags and struct sizes. Build with
 * `make fuzz` (clang). Without libFuzzer, -DSTANDALONE builds a driver that replays files. */
#include <cdin/kernel.h>
#include "support/strict_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rd { const uint8_t *p; size_t n; } rd;
static uint32_t u8(rd *r) { if (!r->n) return 0; r->n--; return *r->p++; }
static uint64_t u16(rd *r) { uint64_t a = u8(r); return a | (uint64_t)u8(r) << 8; }
static const uint8_t *blob(rd *r, size_t *len) { size_t l = u8(r) % 12; const uint8_t *p = r->p; if (l > r->n) l = r->n; *len = l; r->p += l; r->n -= l; return p; }
static uint64_t pos(rd *r) { uint32_t m = u8(r); return m < 200 ? m % 40 : m < 250 ? u16(r) : UINT64_MAX - (m & 3); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    strict_alloc al; cdin_kernel_config c; cdin_kernel *k; rd r = { data, size };
    cdin_doc docs[3] = { 0, 0, 0 }; cdin_txn txns[2] = { 0, 0 }; cdin_snapshot snaps[3] = { 0, 0, 0 }; cdin_notif notifs[2] = { 0, 0 };
    int fail_mode = (int)u8(&r);
    strict_init(&al);
    memset(&c, 0, sizeof c); c.struct_size = sizeof c; c.abi_major = CDIN_KERNEL_ABI_MAJOR; c.allocator = strict_vtable(&al);
    c.max_undo_memory = 200 + u16(&r) % 2000; c.max_document_size = 1 << 16; c.notif_queue_capacity = 4 + u8(&r) % 8; c.max_snapshots_per_doc = 2;
    c.max_blob_size = 16; c.default_search_steps = 5000; c.max_documents = 3; c.max_subscribers_per_doc = 2;
    if (cdin_kernel_create(&c, &k) != CDIN_OK) return 0;
    if (fail_mode & 1) strict_fail_nth(&al, (unsigned)(1 + (fail_mode >> 1) % 64), fail_mode & 0x80);
    while (r.n) {
        uint32_t op = u8(&r) % 24; cdin_doc d = docs[u8(&r) % 3]; cdin_edit_result o;
        memset(&o, 0, sizeof o); o.struct_size = sizeof o;
        switch (op) {
        case 0: { size_t l; const uint8_t *b = blob(&r, &l); cdin_doc nd; if (cdin_doc_create(k, b, l, &nd) == CDIN_OK) { unsigned i = u8(&r) % 3; if (docs[i]) cdin_doc_close(k, docs[i]); docs[i] = nd; } break; }
        case 1: case 2: case 3: { cdin_replace_request q; size_t l; memset(&q, 0, sizeof q); q.struct_size = sizeof q;
            q.start = pos(&r); q.end = pos(&r); q.bytes = blob(&r, &l); q.len = l; q.expected_version = u8(&r) < 200 ? CDIN_ANY_VERSION : u8(&r);
            q.coalesce_key = u8(&r) % 3; if (u8(&r) & 1) { q.blob_before = q.bytes; q.blob_before_len = l; }
            q.flags = (u8(&r) % 32 == 0); if (u8(&r) % 40 == 0) q.struct_size = u8(&r) % sizeof q;   /* shrinking only: claiming more memory than exists is caller UB */
            cdin_doc_replace(k, d, &q, &o); break; }
        case 4: { cdin_history_request h; memset(&h, 0, sizeof h); h.struct_size = sizeof h; h.expected_version = CDIN_ANY_VERSION; cdin_doc_undo(k, d, &h, &o); break; }
        case 5: { cdin_history_request h; uint8_t bo[4]; memset(&h, 0, sizeof h); h.struct_size = sizeof h; h.expected_version = CDIN_ANY_VERSION; h.blob_out = bo; h.blob_capacity = u8(&r) % 6; cdin_doc_redo(k, d, &h, &o); break; }
        case 6: { cdin_txn_begin_request b; unsigned i = u8(&r) % 2; cdin_txn t; memset(&b, 0, sizeof b); b.struct_size = sizeof b; b.expected_version = CDIN_ANY_VERSION;
            if (cdin_txn_begin(k, d, &b, &t) == CDIN_OK) txns[i] = t;
            break; }
        case 7: { cdin_replace_request q; size_t l; memset(&q, 0, sizeof q); q.struct_size = sizeof q; q.expected_version = CDIN_ANY_VERSION;
            q.start = pos(&r); q.end = pos(&r); q.bytes = blob(&r, &l); q.len = l; cdin_txn_replace(k, txns[u8(&r) % 2], &q, &o); break; }
        case 8: { cdin_txn_commit_request q; memset(&q, 0, sizeof q); q.struct_size = sizeof q; cdin_txn_commit(k, txns[u8(&r) % 2], &q, &o); break; }
        case 9: cdin_txn_abort(k, txns[u8(&r) % 2]); break;
        case 10: { uint8_t buf[32]; size_t n; cdin_txn_read(k, txns[u8(&r) % 2], pos(&r), pos(&r), buf, sizeof buf, &n); break; }
        case 11: { unsigned i = u8(&r) % 3; cdin_snapshot s; cdin_version v; if (cdin_snapshot_acquire(k, d, &v, &s) == CDIN_OK) { if (snaps[i]) cdin_snapshot_release(k, snaps[i]); snaps[i] = s; } break; }
        case 12: { unsigned i = u8(&r) % 3; cdin_snapshot_release(k, snaps[i]); snaps[i] = 0; break; }
        case 13: { cdin_subscribe_request q; cdin_notif n; cdin_version v; unsigned i = u8(&r) % 2; memset(&q, 0, sizeof q); q.struct_size = sizeof q;
            if (cdin_notif_subscribe(k, d, &q, &v, &n) == CDIN_OK) { if (notifs[i]) cdin_notif_unsubscribe(k, notifs[i]); notifs[i] = n; } break; }
        case 14: { cdin_notif_event ev[4]; size_t n; cdin_notif_drain(k, notifs[u8(&r) % 2], ev, 1 + u8(&r) % 4, &n); break; }
        case 15: { uint8_t buf[16]; size_t n; uint32_t f; cdin_source s = u8(&r) & 1 ? cdin_src_snap(snaps[u8(&r) % 3]) : cdin_src_doc(d);
            cdin_text_read(k, s, pos(&r), pos(&r), buf, sizeof buf, &n, &f); break; }
        case 16: { cdin_source s = cdin_src_doc(d); uint64_t a, b2; uint32_t f; int raw; cdin_text_line_count(k, s, &a); cdin_text_line_from_offset(k, s, pos(&r), &a, &b2);
            cdin_text_byte_to_codepoint(k, s, pos(&r), &a, &f); cdin_text_codepoint_to_byte(k, s, pos(&r), &a, &f); cdin_text_region_at(k, s, pos(&r), &a, &b2, &raw); break; }
        case 17: { cdin_source s = cdin_src_doc(d); uint64_t a; uint64_t p = pos(&r); cdin_text_grapheme_next(k, s, p, &a); cdin_text_grapheme_prev(k, s, p, &a);
            cdin_text_codepoint_next(k, s, p, &a); cdin_text_codepoint_prev(k, s, p, &a); break; }
        case 18: { cdin_search_request q; cdin_search_result sr; uint64_t rg[8]; size_t l; uint32_t cancel = 0; memset(&q, 0, sizeof q); q.struct_size = sizeof q;
            q.src = u8(&r) & 1 ? cdin_src_snap(snaps[u8(&r) % 3]) : cdin_src_doc(d); q.expected_version = CDIN_ANY_VERSION; q.pattern = blob(&r, &l); q.pattern_len = l;
            q.flags = u8(&r) % 8; q.mode = (cdin_search_mode)(u8(&r) % 3); q.direction = (cdin_search_direction)(u8(&r) & 1); q.from = pos(&r); q.bound = pos(&r);
            q.limit = 1 + u8(&r) % 8; q.ranges = rg; q.ranges_capacity = 8; q.step_limit = u8(&r) % 3 ? 0 : u8(&r); q.cancel = u8(&r) % 16 ? NULL : &cancel;
            memset(&sr, 0, sizeof sr); sr.struct_size = sizeof sr; cdin_search(k, &q, &sr); break; }
        case 19: cdin_doc_mark_clean(k, d); break;
        case 20: { cdin_reload_request q; size_t l; memset(&q, 0, sizeof q); q.struct_size = sizeof q; q.expected_version = CDIN_ANY_VERSION; q.bytes = blob(&r, &l); q.len = l; cdin_doc_reload(k, d, &q, &o); break; }
        case 21: { cdin_batch_request q; uint64_t st[3], en[3]; const uint8_t *by[3]; size_t ln[3]; memset(&q, 0, sizeof q); q.struct_size = sizeof q; q.expected_version = CDIN_ANY_VERSION;
            q.count = u8(&r) % 4; for (int i = 0; i < 3; i++) { st[i] = pos(&r); en[i] = pos(&r); by[i] = blob(&r, &ln[i]); }
            q.starts = st; q.ends = en; q.bytes = by; q.lens = ln; cdin_doc_replace_batch(k, d, &q, &o); break; }
        case 22: { unsigned i = u8(&r) % 3; cdin_doc_close(k, docs[i]); docs[i] = 0; break; }
        default: if (u8(&r) % 50 == 0) { cdin_kernel_shutdown(k, CDIN_SHUTDOWN_GRACEFUL); } break;
        }
    }
    cdin_kernel_destroy(k);
    if (strict_check(&al, "fuzz")) abort();
    return 0;
}

#ifdef STANDALONE
int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--random") == 0) {      /* poor man's fuzzing: N random programs */
        uint64_t seed = strtoull(argv[2], NULL, 10), n = strtoull(argv[3], NULL, 10), x = seed * 0x9E3779B97F4A7C15ull + 1;
        uint8_t buf[4096];
        for (uint64_t it = 0; it < n; it++) {
            size_t len = 1 + (size_t)((x >> 8) % sizeof buf);
            for (size_t j = 0; j < len; j++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; buf[j] = (uint8_t)(x >> 24); if ((x & 7) == 0) buf[j] %= 24; }
            LLVMFuzzerTestOneInput(buf, len);
        }
        printf("fuzz smoke: %llu random programs\n", (unsigned long long)n);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); uint8_t *b; long n;
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0; fclose(f);
        LLVMFuzzerTestOneInput(b, (size_t)n); free(b);
    }
    return 0;
}
#endif
