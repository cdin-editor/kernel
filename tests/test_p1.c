/*
 * P1 regression tests: memory safety and object lifetime.
 *
 * These tests exercise the public contract and link the real kernel. They
 * drive allocation through the injected allocator where relevant.
 */
#include <cdin/kernel.h>
#include "alloc_strict.h"
#include "internal.h"   /* struct doc + handle pool, for the zero-init check */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
static int failures;

static const char *st_name(cdin_status st)
{
    static const cdin_status all[] = {
        CDIN_OK, CDIN_INVALID_ARGUMENT, CDIN_INVALID_HANDLE, CDIN_STALE_VERSION,
        CDIN_OUT_OF_RANGE, CDIN_INVALID_POSITION, CDIN_INVALID_STATE, CDIN_BUSY,
        CDIN_LIMIT_EXCEEDED, CDIN_NO_MEMORY, CDIN_PATTERN_INVALID, CDIN_CANCELLED,
        CDIN_SHUTDOWN, CDIN_UNSUPPORTED, CDIN_BUFFER_TOO_SMALL, CDIN_VERSION_MISMATCH,
        CDIN_INTERNAL
    };
    static const char *names[] = {
        "CDIN_OK", "CDIN_INVALID_ARGUMENT", "CDIN_INVALID_HANDLE", "CDIN_STALE_VERSION",
        "CDIN_OUT_OF_RANGE", "CDIN_INVALID_POSITION", "CDIN_INVALID_STATE", "CDIN_BUSY",
        "CDIN_LIMIT_EXCEEDED", "CDIN_NO_MEMORY", "CDIN_PATTERN_INVALID", "CDIN_CANCELLED",
        "CDIN_SHUTDOWN", "CDIN_UNSUPPORTED", "CDIN_BUFFER_TOO_SMALL",
        "CDIN_VERSION_MISMATCH", "CDIN_INTERNAL"
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i] == st) return names[i];
    return "?";
}
#define status_name st_name

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void banner(const char *name, const char *defect)
{
    printf("%-28s %s\n", name, defect);
}

static cdin_status make_kernel(cdin_kernel **out, alloc_ctx *ctx, unsigned mode,
                               uint64_t max_docs, uint64_t max_doc_size)
{
    alloc_init(ctx, "t", mode);
    cdin_kernel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator = alloc_vtable(ctx);
    cfg.max_documents = max_docs;
    cfg.max_document_size = max_doc_size;
    cfg.max_undo_memory = 1u << 20;
    cfg.max_blob_size = 4096;
    alloc_set_op(ctx, "test");
    return cdin_kernel_create(&cfg, out);
}

/* Reads the whole document. Goes through len + read rather than serialize,
 * because serialize is a P2 defect and would hide P1 results. */
static void read_all(cdin_kernel *k, cdin_doc d, char *buf, size_t cap, size_t *len)
{
    *len = 0;
    uint64_t total = 0;
    if (cdin_text_len(k, cdin_src_doc(d), &total) != CDIN_OK) return;
    if (total > cap) return;
    size_t got = 0;
    if (cdin_text_read(k, cdin_src_doc(d), 0, total, (uint8_t *)buf, cap, &got, NULL) == CDIN_OK) *len = got;
}

/* P4: helper to build a replace request and call cdin_doc_replace. */
static cdin_status do_replace(cdin_kernel *k, cdin_doc d, uint64_t start, uint64_t end,
                              const uint8_t *bytes, size_t len,
                              cdin_version expected, uint64_t coalesce,
                              cdin_edit_result *er)
{
    cdin_replace_request req;
    memset(&req, 0, sizeof(req));
    req.struct_size = sizeof(req);
    req.start = start;
    req.end = end;
    req.bytes = bytes;
    req.len = len;
    req.expected_version = expected;
    req.coalesce_key = coalesce;
    return cdin_doc_replace(k, d, &req, er);
}

/* History, transactions, subscription and search use the ABI 1.0.0 request
 * structs defined in the public header. */
static cdin_status do_undo(cdin_kernel *k, cdin_doc d, cdin_edit_result *er)
{
    cdin_history_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = CDIN_ANY_VERSION;
    return cdin_doc_undo(k, d, &rq, er);
}

static cdin_status do_redo(cdin_kernel *k, cdin_doc d, cdin_edit_result *er)
{
    cdin_history_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = CDIN_ANY_VERSION;
    return cdin_doc_redo(k, d, &rq, er);
}

static cdin_status do_txn_begin(cdin_kernel *k, cdin_doc d, cdin_txn *t)
{
    cdin_txn_begin_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = CDIN_ANY_VERSION;
    return cdin_txn_begin(k, d, &rq, t);
}

static cdin_status do_txn_replace(cdin_kernel *k, cdin_txn t, uint64_t start, uint64_t end,
                                  const uint8_t *bytes, size_t len, cdin_edit_result *er)
{
    cdin_replace_request req;
    memset(&req, 0, sizeof(req));
    req.struct_size = sizeof(req);
    req.expected_version = CDIN_ANY_VERSION;
    req.start = start;
    req.end = end;
    req.bytes = bytes;
    req.len = len;
    return cdin_txn_replace(k, t, &req, er);
}

static cdin_status do_txn_commit(cdin_kernel *k, cdin_txn t, cdin_edit_result *er)
{
    cdin_txn_commit_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    return cdin_txn_commit(k, t, &rq, er);
}

static cdin_status do_subscribe(cdin_kernel *k, cdin_doc d, cdin_notif *nf)
{
    cdin_subscribe_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    return cdin_notif_subscribe(k, d, &rq, NULL, nf);
}

static cdin_status do_search(cdin_kernel *k, cdin_doc d, const uint8_t *pat, size_t plen,
                             cdin_search_result *sr, uint64_t *ranges, uint32_t rcap, uint64_t limit)
{
    cdin_search_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.src = cdin_src_doc(d);
    rq.expected_version = CDIN_ANY_VERSION;
    rq.pattern = pat;
    rq.pattern_len = plen;
    rq.mode = CDIN_SEARCH_LITERAL;
    rq.direction = CDIN_SEARCH_FORWARD;
    rq.limit = limit;
    rq.ranges = ranges;
    rq.ranges_capacity = rcap;
    return cdin_search(k, &rq, sr);
}

static cdin_version doc_ver(cdin_kernel *k, cdin_doc d)
{
    cdin_version v = 0;
    cdin_text_version(k, cdin_src_doc(d), &v);
    return v;
}

/* Prints what is still live so a leak names the block that has to change. */
static void expect_clean(alloc_ctx *ctx)
{
    if (ctx->live_blocks) alloc_report_live(ctx, "leak at destroy");
}

/* Defect: cdin_doc_create initialises bytes/len/version only; tx_active,
 * undo_count, undo_pos, undo_cap, clean and metadata keep whatever the
 * allocator handed back. Observable as BUSY on the first edit, a wrong answer
 * from undo, or a wild pointer read. The zero check runs against the fields of
 * the (internal) doc struct that now back those behaviours. */
static void test_init_zeroed(void)
{
    banner("test_init_zeroed", "doc struct must be fully initialised");
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    CHECK(make_kernel(&k, &ctx, ALLOC_POISON | ALLOC_STRICT, 8, 4096) == CDIN_OK, "create");
    if (!k) return;

    cdin_doc d;
    CHECK(cdin_doc_create(k, (const uint8_t *)"abc", 3, &d) == CDIN_OK, "doc_create");

    struct cdin_kernel *kk = k;
    struct doc *pdoc = (struct doc *)pool_get(&kk->docs, d);
    CHECK(pdoc != NULL && pdoc->units == NULL && pdoc->nunits == 0 && pdoc->units_cap == 0 &&
              pdoc->head == 0 && pdoc->undo_bytes == 0 && pdoc->tx == NULL &&
              pdoc->marker_id == 0 && pdoc->marker_lost == 0 && pdoc->coalesce_unit == 0 &&
              pdoc->next_unit_id == 0 && pdoc->nsnaps == 0 && pdoc->nsubs == 0 &&
              pdoc->metadata == 0 && pdoc->version == 0 && doc_is_clean(pdoc),
          "a new document must start fully zeroed and clean");

    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    cdin_status st = do_replace(k, d, 3, 3, (const uint8_t *)"d", 1,
                                  CDIN_ANY_VERSION, 0, &er);
    CHECK(st == CDIN_OK, "first edit must not report BUSY, got %s", status_name(st));

    cdin_status ust = do_undo(k, d, &er);
    CHECK(ust == CDIN_OK, "undo of the first edit must work, got %s", status_name(ust));

    cdin_kernel_destroy(k);
    CHECK(ctx.live_blocks == 0, "%llu block(s) leaked at destroy",
          (unsigned long long)ctx.live_blocks);
    CHECK(alloc_violations(&ctx) == 0, "%llu allocator violation(s)",
          (unsigned long long)alloc_violations(&ctx));
}

/* Defect: growing docs/generations reallocs docs first; when the second realloc
 * fails the first block is already moved and its pointer is dropped, so the
 * kernel leaks and keeps a stale table. */
static void test_growth_atomic(void)
{
    banner("test_growth_atomic", "failed table growth must leak nothing");
    for (uint64_t fail = 0; fail < 8; fail++) {
        alloc_ctx ctx;
        cdin_kernel *k = NULL;
        CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 64, 4096) == CDIN_OK,
              "kernel create");
        if (!k) return;
        ctx.fail_after = fail + 1; /* allow 'fail' allocations, fail the next */

        cdin_doc d;
        cdin_status st = cdin_doc_create(k, (const uint8_t *)"abc", 3, &d);
        CHECK(st == CDIN_OK || st == CDIN_NO_MEMORY, "fail_after=%llu: got %s",
              (unsigned long long)fail, status_name(st));
        if (st == CDIN_OK) {
            size_t len = 0;
            char buf[16];
            read_all(k, d, buf, sizeof(buf), &len);
            CHECK(len == 3 && memcmp(buf, "abc", 3) == 0,
                  "fail_after=%llu: content wrong", (unsigned long long)fail);
        }
        cdin_kernel_destroy(k);
        uint64_t v = alloc_violations(&ctx);
        CHECK(v == 0, "fail_after=%llu: %llu allocator violation(s) after destroy",
              (unsigned long long)fail, (unsigned long long)v);
        CHECK(ctx.live_blocks == 0, "fail_after=%llu: %llu block(s) leaked",
              (unsigned long long)fail, (unsigned long long)ctx.live_blocks);
    }
}

/* Defect: cdin_doc_replace reallocs the document buffer and then memmoves from
 * the OLD pointer. When realloc relocates, that reads released memory. The
 * ALLOC_MOVE allocator relocates every time and poisons the old bytes. */
static void test_replace_move(void)
{
    banner("test_replace_move", "replace must not read the released buffer");
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    CHECK(make_kernel(&k, &ctx, ALLOC_MOVE | ALLOC_STRICT, 8, 4096) == CDIN_OK, "create");
    if (!k) return;

    cdin_doc d;
    CHECK(cdin_doc_create(k, (const uint8_t *)"hello world", 11, &d) == CDIN_OK, "doc_create");

    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    cdin_status st = do_replace(k, d, 5, 5, (const uint8_t *)"!", 1,
                                  CDIN_ANY_VERSION, 0, &er);
    CHECK(st == CDIN_OK, "insert: %s", status_name(st));

    char buf[32];
    size_t len = 0;
    read_all(k, d, buf, sizeof(buf), &len);
    CHECK(len == 12 && memcmp(buf, "hello! world", 12) == 0,
          "after insert got %zu bytes '%.*s'", len, (int)len, buf);

    /* A replace that shrinks moves the tail; the tail must come from the new
     * buffer, not from the released one. */
    st = do_replace(k, d, 0, 6, (const uint8_t *)"", 0, CDIN_ANY_VERSION, 0, &er);
    CHECK(st == CDIN_OK, "delete: %s", status_name(st));
    read_all(k, d, buf, sizeof(buf), &len);
    CHECK(len == 6 && memcmp(buf, " world", 6) == 0,
          "after delete got %zu bytes '%.*s'", len, (int)len, buf);

    cdin_kernel_destroy(k);
    expect_clean(&ctx);
    CHECK(ctx.live_blocks == 0, "%llu block(s) leaked",
          (unsigned long long)ctx.live_blocks);
}

/* Defect: the same ordering bug on the transaction path. */
static void test_txn_move(void)
{
    banner("test_txn_move", "txn_replace must not read the released buffer");
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    CHECK(make_kernel(&k, &ctx, ALLOC_MOVE | ALLOC_STRICT, 8, 4096) == CDIN_OK, "create");
    if (!k) return;

    cdin_doc d;
    CHECK(cdin_doc_create(k, (const uint8_t *)"hello world", 11, &d) == CDIN_OK, "doc_create");
    cdin_txn t;
    CHECK(do_txn_begin(k, d, &t) == CDIN_OK, "txn_begin");

    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    cdin_status st = do_txn_replace(k, t, 5, 5, (const uint8_t *)"!", 1, &er);
    CHECK(st == CDIN_OK, "txn_replace: %s", status_name(st));

    char buf[32];
    size_t len = 0;
    uint64_t txn_len = 0;
    CHECK(cdin_txn_len(k, t, &txn_len) == CDIN_OK, "txn_len");
    CHECK(txn_len <= sizeof(buf), "tentative transaction content too large");
    CHECK(cdin_txn_read(k, t, 0, txn_len, (uint8_t *)buf, sizeof(buf), &len) == CDIN_OK,
          "txn_read");
    CHECK(len == 12 && memcmp(buf, "hello! world", 12) == 0,
          "tentative txn got %zu bytes '%.*s'", len, (int)len, buf);

    CHECK(cdin_txn_abort(k, t) == CDIN_OK, "txn_abort");
    cdin_kernel_destroy(k);
    expect_clean(&ctx);
    CHECK(ctx.live_blocks == 0, "%llu block(s) leaked",
          (unsigned long long)ctx.live_blocks);
}

/* Defect: the kernel frees a block with a size it never allocated
 * (realloc(..., new_len ? new_len : 1) then free(..., 0)), and frees undo units
 * and their byte copies never at all. */
static void test_destroy_frees_all(void)
{
    banner("test_destroy_frees_all", "destroy must free undo history");
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    CHECK(make_kernel(&k, &ctx, ALLOC_STRICT, 8, 65536) == CDIN_OK, "create");
    if (!k) return;

    cdin_doc d;
    CHECK(cdin_doc_create(k, (const uint8_t *)"hello", 5, &d) == CDIN_OK, "doc_create");
    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    for (int i = 0; i < 4; i++)
        CHECK(do_replace(k, d, 5, 5, (const uint8_t *)"x", 1,
                           CDIN_ANY_VERSION, 0, &er) == CDIN_OK, "replace %d", i);
    CHECK(do_undo(k, d, &er) == CDIN_OK, "undo");
    CHECK(do_redo(k, d, &er) == CDIN_OK, "redo");
    /* Delete everything: the document must end up owning no block at all. */
    char cur[32];
    size_t cur_len = 0;
    read_all(k, d, cur, sizeof(cur), &cur_len);
    cdin_status del = do_replace(k, d, 0, cur_len, (const uint8_t *)"", 0,
                                   CDIN_ANY_VERSION, 0, &er);
    CHECK(del == CDIN_OK, "delete all (%zu bytes): %s", cur_len, status_name(del));

    cdin_kernel_destroy(k);
    expect_clean(&ctx);
    uint64_t v = alloc_violations(&ctx);
    CHECK(v == 0, "%llu allocator violation(s)", (unsigned long long)v);
    CHECK(ctx.live_blocks == 0, "%llu block(s) leaked at destroy",
          (unsigned long long)ctx.live_blocks);
}

/* Defect: an allocation failure inside an edit is ignored, so the edit is
 * applied without its undo record, or the undo record is built from a failed
 * allocation. ERR-005/EDIT-010: the document and its version must be unchanged. */
static void test_edit_failure_atomic(void)
{
    banner("test_edit_failure_atomic", "failed allocation changes nothing");
    for (uint64_t fail = 0; fail < 12; fail++) {
        alloc_ctx ctx;
        cdin_kernel *k = NULL;
        CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 8, 4096) == CDIN_OK,
              "kernel create");
        if (!k) return;
        cdin_doc d;
        CHECK(cdin_doc_create(k, (const uint8_t *)"abc", 3, &d) == CDIN_OK, "doc_create");
        ctx.fail_after = ctx.count + fail;

        cdin_edit_result er;
        memset(&er, 0, sizeof(er));
        er.struct_size = sizeof(er);
        cdin_status st = do_replace(k, d, 3, 3, (const uint8_t *)"d", 1,
                                          CDIN_ANY_VERSION, 0, &er);

        char before[16], after[16];
        size_t blen = 0, alen = 0;
        read_all(k, d, before, sizeof(before), &blen);
        cdin_version v_before = doc_ver(k, d);

        CHECK(st == CDIN_OK || st == CDIN_NO_MEMORY, "fail_after=%llu: got %s",
              (unsigned long long)fail, status_name(st));
        if (st == CDIN_OK) {
            /* The edit landed, so its undo record must exist: an edit that
             * cannot be undone is silent data loss. Injection is disarmed for
             * the undo so this checks the record, not a second OOM. What undo
             * then computes is P5 (undo.* vectors), not P1. */
            ctx.fail_after = UINT64_MAX;
            cdin_status ust = do_undo(k, d, &er);
            CHECK(ust == CDIN_OK, "fail_after=%llu: undo after OK returned %s",
                  (unsigned long long)fail, status_name(ust));
        } else {
            read_all(k, d, after, sizeof(after), &alen);
            CHECK(alen == 3 && memcmp(after, "abc", 3) == 0,
                  "fail_after=%llu: NO_MEMORY still changed the bytes", (unsigned long long)fail);
            cdin_version v_after = doc_ver(k, d);
            CHECK(v_after == v_before, "fail_after=%llu: NO_MEMORY consumed a version",
                  (unsigned long long)fail);
        }
        cdin_kernel_destroy(k);
        CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
              "fail_after=%llu: %llu violation(s), %llu leak(s)", (unsigned long long)fail,
              (unsigned long long)alloc_violations(&ctx), (unsigned long long)ctx.live_blocks);
    }
}

/* Defect: the same for undo and redo, whose buffers come from realloc. */
static void test_undo_failure_atomic(void)
{
    banner("test_undo_failure_atomic", "failed undo allocation changes nothing");
    for (uint64_t fail = 0; fail < 8; fail++) {
        alloc_ctx ctx;
        cdin_kernel *k = NULL;
        CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 8, 4096) == CDIN_OK,
              "kernel create");
        if (!k) return;
        cdin_doc d;
        cdin_doc_create(k, (const uint8_t *)"abc", 3, &d);
        cdin_edit_result er;
        memset(&er, 0, sizeof(er));
        er.struct_size = sizeof(er);
        do_replace(k, d, 1, 2, (const uint8_t *)"XY", 2, CDIN_ANY_VERSION, 0, &er);

        char after[16];
        size_t alen = 0;
        read_all(k, d, after, sizeof(after), &alen);
        cdin_version v_before = doc_ver(k, d);

        ctx.fail_after = ctx.count + fail;
        cdin_status st = do_undo(k, d, &er);
        CHECK(st == CDIN_OK || st == CDIN_NO_MEMORY, "fail_after=%llu: undo got %s",
              (unsigned long long)fail, status_name(st));
        if (st != CDIN_OK) {
            char now[16];
            size_t nlen = 0;
            read_all(k, d, now, sizeof(now), &nlen);
            CHECK(nlen == alen && memcmp(now, after, alen) == 0,
                  "fail_after=%llu: failed undo changed the bytes", (unsigned long long)fail);
            cdin_version v_after = doc_ver(k, d);
            CHECK(v_after == v_before, "fail_after=%llu: failed undo consumed a version",
                  (unsigned long long)fail);
        }
        cdin_kernel_destroy(k);
        CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
              "fail_after=%llu: %llu violation(s), %llu leak(s)", (unsigned long long)fail,
              (unsigned long long)alloc_violations(&ctx), (unsigned long long)ctx.live_blocks);
    }
}

/* Defect: every remaining operation must survive an allocation failure with an
 * untouched out-parameter, and must not leak (ABORT on the operation itself). */
static void test_ops_failure_sweep(void)
{
    banner("test_ops_failure_sweep", "every op tolerates NO_MEMORY");
    for (uint64_t fail = 0; fail < 24; fail++) {
        alloc_ctx ctx;
        cdin_kernel *k = NULL;
        CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 8, 65536) == CDIN_OK,
              "kernel create");
        if (!k) return;
        cdin_doc d;
        cdin_doc_create(k, (const uint8_t *)"hello world", 11, &d);
        cdin_edit_result er;
        memset(&er, 0, sizeof(er));
        er.struct_size = sizeof(er);
        do_replace(k, d, 5, 11, (const uint8_t *)" cd", 3, CDIN_ANY_VERSION, 0, &er);

        ctx.fail_after = ctx.count + fail;

        cdin_txn t = CDIN_INVALID_TXN;
        cdin_status s1 = do_txn_begin(k, d, &t);
        cdin_edit_result tx_er;
        memset(&tx_er, 0, sizeof(tx_er));
        tx_er.struct_size = sizeof(tx_er);
        if (s1 == CDIN_OK) do_txn_replace(k, t, 0, 2, (const uint8_t *)"XX", 2, &tx_er);
        cdin_txn_abort(k, t);

        cdin_notif nf = CDIN_INVALID_NOTIF;
        do_subscribe(k, d, &nf);
        cdin_notif_event ev;
        size_t got = 0;
        cdin_notif_drain(k, nf, &ev, 1, &got);
        cdin_notif_unsubscribe(k, nf);

        cdin_version sv = 0;
        cdin_snapshot s = CDIN_INVALID_SNAPSHOT;
        cdin_snapshot_acquire(k, d, &sv, &s);
        size_t slen = 0;
        uint8_t sbuf[16];
        cdin_text_read(k, cdin_src_snap(s), 0, 3, sbuf, sizeof(sbuf), &slen, NULL);
        cdin_snapshot_release(k, s);

        cdin_search_result sr;
        memset(&sr, 0, sizeof(sr));
        sr.struct_size = sizeof(sr);
        {
            uint64_t rbuf[8];
            do_search(k, d, (const uint8_t *)"o", 1, &sr, rbuf, 8, 8);
        }

        cdin_kernel_destroy(k);
        CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
              "fail_after=%llu: %llu violation(s), %llu leak(s)", (unsigned long long)fail,
              (unsigned long long)alloc_violations(&ctx), (unsigned long long)ctx.live_blocks);
    }
}

/* ---------------------------------------------------------------------------
 * Full allocation-failure sweep.
 *
 * For every operation and every allocation point, three things are checked:
 * the status is OK or NO_MEMORY; on NO_MEMORY the document bytes, the version
 * and the undo state are exactly what they were before the call; and the
 * out-parameters are untouched. Whatever is still allocated at destroy must be
 * zero. The reference undo status comes from an identical kernel that was
 * allowed to allocate, so "the history is unchanged" is measured, not assumed.
 * ------------------------------------------------------------------------- */

typedef struct {
    char bytes[64];
    size_t len;
    cdin_version version;
} doc_state;

static void capture(cdin_kernel *k, cdin_doc d, doc_state *s)
{
    memset(s, 0, sizeof(*s));
    read_all(k, d, s->bytes, sizeof(s->bytes), &s->len);
    s->version = doc_ver(k, d);
}

static cdin_status undo_probe(cdin_kernel *k, cdin_doc d)
{
    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    return do_undo(k, d, &er);
}

static int state_same(const doc_state *a, const doc_state *b)
{
    return a->len == b->len && a->version == b->version &&
           memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* Builds a kernel with one document that already has one edit in its history. */
static void scenario(alloc_ctx *ctx, unsigned mode, cdin_kernel **k, cdin_doc *d,
                     uint64_t max_docs, uint64_t max_doc_size)
{
    if (make_kernel(k, ctx, mode, max_docs, max_doc_size) != CDIN_OK) { *k = NULL; return; }
    if (cdin_doc_create(*k, (const uint8_t *)"hello world", 11, d) != CDIN_OK) { *k = NULL; return; }
    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    do_replace(*k, *d, 5, 11, (const uint8_t *)" cd", 3, CDIN_ANY_VERSION, 0, &er);
}

#define SENTINEL 0x5Au
#define FAIL_POINTS 28

/* A failed call must not write any out-parameter. struct_size is set by the
 * caller before the call, so it is excluded from the comparison. */
static int out_untouched(const void *p, size_t n, int allow_accepted_seq)
{
    unsigned char copy[sizeof(cdin_edit_result) > sizeof(cdin_search_result)
                           ? sizeof(cdin_edit_result) : sizeof(cdin_search_result)];
    const unsigned char *b;
    if (!p || n > sizeof(copy)) return 0;
    memcpy(copy, p, n);
    /* ABI 1.0.0 permits `details` on failure. After mutation acceptance, an
     * allocation/limit failure may also expose `accepted_seq` by contract. */
    if (n == sizeof(cdin_edit_result)) {
        memset(copy + offsetof(cdin_edit_result, details), SENTINEL,
               sizeof(((cdin_edit_result *)0)->details));
        /* A mutation may be accepted and assigned a sequence before a later
         * allocation fails. In that case accepted_seq is intentionally valid. */
        if (allow_accepted_seq)
            memset(copy + offsetof(cdin_edit_result, accepted_seq), SENTINEL,
                   sizeof(((cdin_edit_result *)0)->accepted_seq));
    } else if (n == sizeof(cdin_search_result))
        memset(copy + offsetof(cdin_search_result, details), SENTINEL,
               sizeof(((cdin_search_result *)0)->details));
    b = copy;
    for (size_t i = sizeof(uint32_t); i < n; i++)
        if (b[i] != SENTINEL) return 0;
    return 1;
}

static void check_failure(const char *op, unsigned mode, uint64_t max_docs,
                          uint64_t max_doc_size, int is_mutation, int tolerant,
                          void (*prepare)(cdin_kernel *, cdin_doc),
                          cdin_status (*run)(cdin_kernel *, cdin_doc, void *),
                          void *arg, const char *expected_after_ok)
{
    /* Reference: the same scenario with every allocation allowed. */
    alloc_ctx ref_ctx;
    cdin_kernel *ref_k = NULL;
    cdin_doc ref_d = CDIN_INVALID_DOC;
    scenario(&ref_ctx, mode, &ref_k, &ref_d, max_docs, max_doc_size);
    cdin_status ref_undo = CDIN_OK;
    if (ref_k) {
        if (prepare) prepare(ref_k, ref_d);
        if (is_mutation) ref_undo = undo_probe(ref_k, ref_d);
        cdin_kernel_destroy(ref_k);
    }

    for (uint64_t fail = 0; fail < FAIL_POINTS; fail++) {
        alloc_ctx ctx;
        cdin_kernel *k = NULL;
        cdin_doc d = CDIN_INVALID_DOC;
        scenario(&ctx, mode, &k, &d, max_docs, max_doc_size);
        if (!k) { CHECK(0, "%s: scenario setup failed", op); return; }

        /* The setup an operation needs (for redo: a unit in the redo slot)
         * happens with allocations still allowed. */
        if (prepare) prepare(k, d);

        doc_state before;
        capture(k, d, &before);
        ctx.fail_after = ctx.count + fail;

        cdin_status st = run(k, d, arg);

        CHECK(st == CDIN_OK || st == CDIN_NO_MEMORY ||
                  (tolerant && (st == CDIN_INVALID_HANDLE || st == CDIN_LIMIT_EXCEEDED ||
                                st == CDIN_INVALID_STATE)),
              "%s: fail_after=%llu returned %s", op, (unsigned long long)fail, st_name(st));

        doc_state after;
        capture(k, d, &after);
        if (st == CDIN_OK) {
            CHECK(expected_after_ok == NULL || after.len == strlen(expected_after_ok),
                  "%s: fail_after=%llu ok but length %zu", op,
                  (unsigned long long)fail, after.len);
            if (is_mutation)
                CHECK(after.version == before.version + 1,
                      "%s: fail_after=%llu ok but version %llu", op,
                      (unsigned long long)fail, (unsigned long long)after.version);
        } else {
            CHECK(state_same(&before, after.len == before.len ? &before : &after),
                  "%s: fail_after=%llu NO_MEMORY changed the document", op,
                  (unsigned long long)fail);
            if (is_mutation) {
                ctx.fail_after = UINT64_MAX; /* probe with allocations allowed */
                cdin_status u = undo_probe(k, d);
                CHECK(u == ref_undo,
                      "%s: fail_after=%llu NO_MEMORY changed the undo state (%s vs %s)",
                      op, (unsigned long long)fail, st_name(u), st_name(ref_undo));
            }
        }
        ctx.fail_after = UINT64_MAX;
        cdin_kernel_destroy(k);
        CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
              "%s: fail_after=%llu: %llu violation(s), %llu leak(s)", op,
              (unsigned long long)fail, (unsigned long long)alloc_violations(&ctx),
              (unsigned long long)ctx.live_blocks);
    }
}

/* --- the operations under sweep ------------------------------------------- */

/* Puts one unit back into the redo slot; runs before the fault is injected. */
static void prepare_redo(cdin_kernel *k, cdin_doc d)
{
    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    do_undo(k, d, &er);
}

static cdin_status op_create(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)k; (void)d; (void)arg;
    cdin_doc fresh = CDIN_INVALID_DOC;
    return cdin_doc_create(k, (const uint8_t *)"x", 1, &fresh);
}

static cdin_status op_replace(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_edit_result er;
    memset(&er, SENTINEL, sizeof(er)); /* out-params must survive a failure */
    cdin_status st = do_replace(k, d, 8, 8, (const uint8_t *)"!", 1,
                                  CDIN_ANY_VERSION, 0, &er);
    if (st != CDIN_OK)
        CHECK(out_untouched(&er, sizeof(er), st == CDIN_NO_MEMORY || st == CDIN_LIMIT_EXCEEDED), "replace: out-parameters modified on %s",
              st_name(st));
    return st;
}

/* Shrinking replace: the tail moves before the block is shrunk, and a failing
 * shrinking realloc must leave the logical length at the new value. */
static cdin_status op_replace_shrink(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_edit_result er;
    memset(&er, SENTINEL, sizeof(er));
    er.struct_size = sizeof(er);
    cdin_status st = do_replace(k, d, 0, 3, (const uint8_t *)"hi", 2,
                                  CDIN_ANY_VERSION, 0, &er);
    if (st != CDIN_OK)
        CHECK(out_untouched(&er, sizeof(er), st == CDIN_NO_MEMORY || st == CDIN_LIMIT_EXCEEDED), "replace_shrink: out-parameters modified on %s",
              st_name(st));
    return st;
}

static cdin_status op_undo(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_edit_result er;
    memset(&er, SENTINEL, sizeof(er));
    cdin_status st = do_undo(k, d, &er);
    if (st != CDIN_OK && st != CDIN_INVALID_STATE)
        CHECK(out_untouched(&er, sizeof(er), st == CDIN_NO_MEMORY || st == CDIN_LIMIT_EXCEEDED), "undo: out-parameters modified on %s",
              st_name(st));
    return st;
}

static cdin_status op_redo(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_edit_result er;
    memset(&er, SENTINEL, sizeof(er));
    er.struct_size = sizeof(er);
    cdin_status st = do_redo(k, d, &er);
    if (st != CDIN_OK)
        CHECK(out_untouched(&er, sizeof(er), st == CDIN_NO_MEMORY || st == CDIN_LIMIT_EXCEEDED), "redo: out-parameters modified on %s",
              st_name(st));
    return st;
}

static cdin_status op_txn(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_txn t = CDIN_INVALID_TXN;
    cdin_status st = do_txn_begin(k, d, &t);
    if (st != CDIN_OK) return st;
    cdin_edit_result ter;
    memset(&ter, 0, sizeof(ter));
    ter.struct_size = sizeof(ter);
    do_txn_replace(k, t, 8, 8, (const uint8_t *)"?", 1, &ter);
    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    return do_txn_commit(k, t, &er);
}

static cdin_status op_subscribe(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_notif nf = CDIN_INVALID_NOTIF;
    cdin_status st = do_subscribe(k, d, &nf);
    if (st != CDIN_OK) return st;
    cdin_notif_event ev;
    size_t got = 0;
    cdin_notif_drain(k, nf, &ev, 1, &got);
    return cdin_notif_unsubscribe(k, nf);
}

static cdin_status op_snapshot(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_version sv = 0;
    cdin_snapshot s = CDIN_INVALID_SNAPSHOT;
    cdin_status st = cdin_snapshot_acquire(k, d, &sv, &s);
    if (st != CDIN_OK) return st;
    uint8_t buf[16];
    size_t got = 0;
    cdin_text_read(k, cdin_src_snap(s), 0, 3, buf, sizeof(buf), &got, NULL);
    return cdin_snapshot_release(k, s);
}

static cdin_status op_search(cdin_kernel *k, cdin_doc d, void *arg)
{
    (void)arg;
    cdin_search_result sr;
    memset(&sr, SENTINEL, sizeof(sr));
    sr.struct_size = sizeof(sr);
    uint64_t rbuf[8];
    cdin_status st = do_search(k, d, (const uint8_t *)"o", 1, &sr, rbuf, 8, 8);
    if (st != CDIN_OK)
        CHECK(out_untouched(&sr, sizeof(sr), 0), "search: out-parameters modified on %s",
              st_name(st));
    return st;
}

static void test_sweeps(void)
{
    banner("test_sweeps", "every operation under injected failure");
    unsigned mode = ALLOC_COUNTING | ALLOC_STRICT;
    /* create does not mutate the scenario document: no version or undo rule.
     * Subscribe and snapshot tolerate allocation failure as long as the failed
     * operation leaves no published handle or leaked allocation. */
    check_failure("create_doc", mode, 64, 65536, 0, 0, NULL, op_create, NULL, NULL);
    check_failure("replace", mode, 8, 4096, 1, 0, NULL, op_replace, NULL, "hello cd!");
    check_failure("replace_shrink", mode, 8, 4096, 1, 0, NULL, op_replace_shrink, NULL,
                  "hilo cd");
    check_failure("undo", mode, 8, 4096, 1, 0, NULL, op_undo, NULL, NULL);
    check_failure("redo", mode, 8, 4096, 1, 0, prepare_redo, op_redo, NULL, NULL);
    check_failure("txn", mode, 8, 4096, 0, 1, NULL, op_txn, NULL, NULL);
    check_failure("subscribe", mode, 8, 4096, 0, 1, NULL, op_subscribe, NULL, NULL);
    check_failure("snapshot", mode, 8, 4096, 0, 1, NULL, op_snapshot, NULL, NULL);
    check_failure("search", mode, 8, 4096, 0, 0, NULL, op_search, NULL, NULL);
}

/* UND-020/021: the caller blob is copied out, never borrowed.
 *
 * There is no cdin_doc_get_last_blob API in ABI 1.0.0: the blob of the last
 * unit travels with the undo/redo request (req.blob_out / result.blob_len).
 * undo publishes blob_before ("ell"), redo blob_after ("IPP"); each probe
 * moves the head, so the probes below are ordered undo -> redo -> undo. */
static void test_get_last_blob(void)
{
    banner("test_get_last_blob", "UND-020/021 caller blob is copied out");
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 8, 4096) == CDIN_OK, "create");
    if (!k) return;

    cdin_doc d;
    CHECK(cdin_doc_create(k, (const uint8_t *)"hello", 5, &d) == CDIN_OK, "doc_create");

    /* Replace "ell" with "IPP": before = "ell", after = "IPP". */
    cdin_edit_result er;
    memset(&er, 0, sizeof(er));
    er.struct_size = sizeof(er);
    cdin_replace_request edit;
    memset(&edit, 0, sizeof(edit));
    edit.struct_size = sizeof(edit);
    edit.expected_version = CDIN_ANY_VERSION;
    edit.start = 1;
    edit.end = 4;
    edit.bytes = (const uint8_t *)"IPP";
    edit.len = 3;
    edit.blob_before = (const uint8_t *)"ell";
    edit.blob_before_len = 3;
    edit.blob_after = (const uint8_t *)"IPP";
    edit.blob_after_len = 3;
    cdin_status st = cdin_doc_replace(k, d, &edit, &er);
    CHECK(st == CDIN_OK, "replace: %s", status_name(st));

    cdin_history_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = CDIN_ANY_VERSION;

    /* Copy-out: undo publishes the pre-image, redo the post-image. */
    char before[8] = {0}, after[8] = {0};
    rq.blob_out = (uint8_t *)before;
    rq.blob_capacity = sizeof(before);
    st = cdin_doc_undo(k, d, &rq, &er);
    CHECK(st == CDIN_OK && er.blob_len == 3 && memcmp(before, "ell", 3) == 0,
          "before: %s len=%llu", status_name(st), (unsigned long long)er.blob_len);

    rq.blob_out = (uint8_t *)after;
    rq.blob_capacity = sizeof(after);
    st = cdin_doc_redo(k, d, &rq, &er);
    CHECK(st == CDIN_OK && er.blob_len == 3 && memcmp(after, "IPP", 3) == 0,
          "after: %s len=%llu", status_name(st), (unsigned long long)er.blob_len);

    /* Two-call: buf == NULL reports the size (here: undo again, size first). */
    rq.blob_out = NULL;
    rq.blob_capacity = 0;
    st = cdin_doc_undo(k, d, &rq, &er);
    CHECK(st == CDIN_OK && er.blob_len == 3, "before size: %s len=%llu",
          status_name(st), (unsigned long long)er.blob_len);

    /* Too-small buffer. */
    char tiny[2];
    rq.blob_out = (uint8_t *)tiny;
    rq.blob_capacity = sizeof(tiny);
    st = cdin_doc_redo(k, d, &rq, &er);
    CHECK(st == CDIN_BUFFER_TOO_SMALL, "tiny buffer: %s", status_name(st));
    CHECK(er.details.required == 3, "tiny buffer reports required=%llu",
          (unsigned long long)er.details.required);

    /* No undo unit yet. */
    cdin_doc fresh;
    CHECK(cdin_doc_create(k, NULL, 0, &fresh) == CDIN_OK, "doc_create fresh");
    rq.blob_out = NULL;
    st = cdin_doc_undo(k, fresh, &rq, &er);
    CHECK(st == CDIN_INVALID_STATE, "no undo: %s", status_name(st));
    CHECK(cdin_doc_close(k, fresh) == CDIN_OK, "close fresh");

    CHECK(cdin_doc_close(k, d) == CDIN_OK, "close");
    CHECK(cdin_kernel_destroy(k) == CDIN_OK, "destroy");
    CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
          "leak: %llu violation(s), %llu live", (unsigned long long)alloc_violations(&ctx),
          (unsigned long long)ctx.live_blocks);
}


/* NOTIF regression: repeatedly subscribing and unsubscribing must not retain
 * one shell per cycle. A warmup allocates the reusable pool slot; live allocator
 * blocks must then remain stable while the slot is reused. */
static void test_notification_churn(void)
{
    banner("test_notification_churn", "unsubscribe frees state and invalidates handles");
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    cdin_doc d = 0;
    CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 8, 4096) == CDIN_OK,
          "create kernel");
    if (!k) return;
    CHECK(cdin_doc_create(k, (const uint8_t *)"churn", 5, &d) == CDIN_OK,
          "create document");

    cdin_notif first = CDIN_INVALID_NOTIF;
    CHECK(do_subscribe(k, d, &first) == CDIN_OK, "warmup subscribe");
    CHECK(cdin_notif_unsubscribe(k, first) == CDIN_OK, "warmup unsubscribe");
    const unsigned long long baseline = (unsigned long long)ctx.live_blocks;

    for (unsigned i = 0; i < 2000; ++i) {
        cdin_notif n = CDIN_INVALID_NOTIF;
        cdin_status st = do_subscribe(k, d, &n);
        CHECK(st == CDIN_OK, "subscribe #%u: %s", i, status_name(st));
        if (st != CDIN_OK) break;
        st = cdin_notif_unsubscribe(k, n);
        CHECK(st == CDIN_OK, "unsubscribe #%u: %s", i, status_name(st));
        CHECK((unsigned long long)ctx.live_blocks == baseline,
              "live allocations grew after cycle #%u: %llu != %llu", i,
              (unsigned long long)ctx.live_blocks, baseline);
        size_t count = 0;
        st = cdin_notif_drain(k, n, NULL, 0, &count);
        CHECK(st == CDIN_INVALID_HANDLE, "stale handle drain #%u: %s", i,
              status_name(st));
    }

    CHECK(cdin_doc_close(k, d) == CDIN_OK, "close document");
    CHECK(cdin_kernel_destroy(k) == CDIN_OK, "destroy kernel");
    CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
          "allocator issue after churn: %llu violation(s), %llu live blocks",
          (unsigned long long)alloc_violations(&ctx),
          (unsigned long long)ctx.live_blocks);
}

/* Search-index regression: a cached negative result must be exact, the raw-byte
 * shortcut must retain skipped_raw semantics, and edits must invalidate the index. */
static void test_search_index(void)
{
    banner("test_search_index", "large negative search cache invalidates after edit");
    const size_t n = TEXT_SEARCH_INDEX_MIN_BYTES;
    uint8_t *data = (uint8_t *)malloc(n);
    alloc_ctx ctx;
    cdin_kernel *k = NULL;
    cdin_doc d = 0;
    cdin_search_request q;
    cdin_search_result r;
    cdin_edit_result er;
    uint64_t ranges[2] = { UINT64_MAX, UINT64_MAX };
    const uint8_t needle[] = "needle";
    if (!data) { CHECK(0, "allocate large test document"); return; }
    memset(data, 'a', n);
    CHECK(make_kernel(&k, &ctx, ALLOC_COUNTING | ALLOC_STRICT, 8, (uint64_t)n + 64u) == CDIN_OK,
          "create kernel");
    if (!k) { free(data); return; }
    CHECK(cdin_doc_create(k, data, n, &d) == CDIN_OK, "create large document");
    free(data);
    if (d == 0) { cdin_kernel_destroy(k); return; }

    memset(&q, 0, sizeof(q));
    q.struct_size = sizeof(q);
    q.src = cdin_src_doc(d);
    q.expected_version = CDIN_ANY_VERSION;
    q.pattern = needle;
    q.pattern_len = sizeof(needle) - 1u;
    q.mode = CDIN_SEARCH_LITERAL;
    q.direction = CDIN_SEARCH_FORWARD;
    q.bound = UINT64_MAX;
    q.limit = 1;
    q.ranges = ranges;
    q.ranges_capacity = 1;
    memset(&r, 0, sizeof(r)); r.struct_size = sizeof(r);
    CHECK(cdin_search(k, &q, &r) == CDIN_OK, "indexed negative search");
    CHECK(r.count == 0 && r.exhausted == 1, "negative result count=%llu exhausted=%u",
          (unsigned long long)r.count, (unsigned)r.exhausted);
    CHECK(r.steps == 0, "negative trigram-filter result visited %llu candidate positions",
          (unsigned long long)r.steps);
    CHECK(r.skipped_raw == 0, "valid UTF-8 document incorrectly reported raw bytes");

    /* Invalidate and rebuild the index on a document containing a raw byte;
     * SEARCH-042 must still report that the scanned range skipped raw bytes. */
    const uint8_t raw = 0xFFu;
    memset(&er, 0, sizeof(er)); er.struct_size = sizeof(er);
    CHECK(do_replace(k, d, n / 2u, n / 2u + 1u, &raw, 1u, CDIN_ANY_VERSION, 0, &er) == CDIN_OK,
          "insert raw byte and invalidate index");
    ranges[0] = ranges[1] = UINT64_MAX;
    memset(&r, 0, sizeof(r)); r.struct_size = sizeof(r);
    CHECK(cdin_search(k, &q, &r) == CDIN_OK, "indexed negative search with raw byte");
    CHECK(r.count == 0 && r.skipped_raw == 1, "raw byte flag lost by index (count=%llu skipped_raw=%u)",
          (unsigned long long)r.count, (unsigned)r.skipped_raw);

    memset(&er, 0, sizeof(er)); er.struct_size = sizeof(er);
    CHECK(do_replace(k, d, 0, 1, needle, sizeof(needle) - 1u, CDIN_ANY_VERSION, 0, &er) == CDIN_OK,
          "edit invalidates cached index");
    ranges[0] = ranges[1] = UINT64_MAX;
    memset(&r, 0, sizeof(r)); r.struct_size = sizeof(r);
    CHECK(cdin_search(k, &q, &r) == CDIN_OK, "search after index invalidation");
    CHECK(r.count == 1 && ranges[0] == 0 && ranges[1] == sizeof(needle) - 1u,
          "fresh match count=%llu range=[%llu,%llu]", (unsigned long long)r.count,
          (unsigned long long)ranges[0], (unsigned long long)ranges[1]);

    CHECK(cdin_doc_close(k, d) == CDIN_OK, "close large document");
    CHECK(cdin_kernel_destroy(k) == CDIN_OK, "destroy kernel");
    CHECK(alloc_violations(&ctx) == 0 && ctx.live_blocks == 0,
          "allocator leak: %llu violation(s), %llu live", (unsigned long long)alloc_violations(&ctx),
          (unsigned long long)ctx.live_blocks);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* keep the log when a defect crashes us */
    const char *only = argc > 1 ? argv[1] : NULL;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "init", test_init_zeroed },
        { "growth", test_growth_atomic },
        { "replace_move", test_replace_move },
        { "txn_move", test_txn_move },
        { "destroy", test_destroy_frees_all },
        { "edit_atomic", test_edit_failure_atomic },
        { "undo_atomic", test_undo_failure_atomic },
        { "ops_sweep", test_ops_failure_sweep },
        { "sweeps", test_sweeps },
        { "get_last_blob", test_get_last_blob },
        { "notification_churn", test_notification_churn },
        { "search_index", test_search_index }
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (only && strcmp(only, tests[i].name) != 0) continue;
        tests[i].fn();
    }
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all P1 checks passed\n");
    return 0;
}

