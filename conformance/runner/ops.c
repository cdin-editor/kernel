/* Vector operations. One entry per operation name; each declares the keys it
 * accepts (unknown or duplicate keys are syntax errors) and writes its results
 * into the shared output block that `expect` reads. */
#include "ops.h"
#include "parser.h"
#include "runner_state.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    vec_arg args[VEC_MAX_ARGS];
    size_t n;
    vec_ctx ctx;
    const char *op;
} opin;

static int syntax(opin *in, const char *what, const char *detail)
{
    if (detail) fprintf(stderr, "%s:%d: syntax error: %s '%s'\n", in->ctx.path, in->ctx.line, what, detail);
    else fprintf(stderr, "%s:%d: syntax error: %s\n", in->ctx.path, in->ctx.line, what);
    (*in->ctx.errors)++;
    return -1;
}

/* Returns 1 and stores the document handle, or 0 when the vector named a
 * handle that was never issued (the caller reports INVALID_HANDLE). */
static int doc_arg(opin *in, cdin_doc *out, const char **name_out)
{
    const char *name = vec_kv_req(in->args, in->n, "handle", &in->ctx);
    if (!name) return 0;
    if (name_out) *name_out = name;
    const handle_entry *h = handle_get(name, HK_DOC);
    if (!h) return 0;
    *out = (cdin_doc)h->value;
    return 1;
}

static int handle_arg(opin *in, handle_kind kind, const handle_entry **out)
{
    const char *name = vec_kv_req(in->args, in->n, "handle", &in->ctx);
    if (!name) return 0;
    const handle_entry *h = handle_get(name, kind);
    if (!h) return 0;
    *out = h;
    return 1;
}

static uint64_t u64_arg(opin *in, const char *key, uint64_t dflt, int *ok)
{
    const char *v = vec_kv(in->args, in->n, key, &in->ctx);
    if (!v) { *ok = 1; return dflt; }
    uint64_t out = 0;
    if (vec_u64(v, &out) != 0) { syntax(in, "invalid number for", key); *ok = 0; return 0; }
    *ok = 1;
    return out;
}

static int u64_req(opin *in, const char *key, uint64_t *out)
{
    const char *v = vec_kv_req(in->args, in->n, key, &in->ctx);
    if (!v) return -1;
    if (vec_u64(v, out) != 0) return syntax(in, "invalid number for", key);
    return 0;
}

static int bytes_arg(opin *in, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    const char *v = vec_kv_req(in->args, in->n, key, &in->ctx);
    if (!v) return -1;
    if (vec_bytes(v, buf, cap, len) != 0) return syntax(in, "byte string too long for", key);
    return 0;
}

static void test_wakeup(void *ctx) { (void)ctx; }

/* Publishes the edit result fields the kernel filled in, for `expect`. */
static void publish_edit(void)
{
    g_out.num_new_version = g_out.edit.new_version;
    g_out.num_seq = g_out.edit.accepted_seq;
    g_out.num_start = g_out.edit.start;
    g_out.num_removed = g_out.edit.removed;
    g_out.num_inserted = g_out.edit.inserted;
    g_out.num_new_length = g_out.edit.new_length;
}

static int op_create(opin *in)
{
    const char *name = vec_kv_req(in->args, in->n, "allocator", &in->ctx);
    if (!name) return -1;
    int ok = 1;
    uint64_t max_docs = u64_arg(in, "max_documents", 64, &ok);
    uint64_t max_doc = u64_arg(in, "max_document_size", 65536, &ok);
    uint64_t max_undo = u64_arg(in, "max_undo", 1048576, &ok);
    uint64_t max_blob = u64_arg(in, "max_blob", 65536, &ok);
    if (!ok) return -1;

    alloc_ctx *ctx = allocator_get(name);
    if (!ctx) return syntax(in, "unknown allocator", name);

    cdin_kernel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.allocator = alloc_vtable(ctx);
    cfg.max_documents = max_docs;
    cfg.max_document_size = max_doc;
    cfg.max_undo_memory = max_undo;
    cfg.max_blob_size = max_blob;

    cdin_kernel *k = NULL;
    out_begin();
    g_last_status = cdin_kernel_create(&cfg, &k);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("create", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    g_kernel = k;
    if (!handle_new(HK_KERNEL, k, (uint64_t)(uintptr_t)k)) {
        cdin_kernel_destroy(k);
        return syntax(in, "handle table is full", NULL);
    }
    return 0;
}

static int op_destroy(opin *in)
{
    const handle_entry *h;
    if (!handle_arg(in, HK_KERNEL, &h)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    cdin_kernel *k = h->k;
    out_begin();
    g_last_status = cdin_kernel_destroy(k);
    handle_drop(h->name);
    if (g_kernel == k) g_kernel = NULL;
    return 0;
}

static int op_create_doc(opin *in)
{
    uint8_t bytes[4096];
    size_t len = 0;
    const char *v = vec_kv(in->args, in->n, "bytes", &in->ctx);
    if (v && vec_bytes(v, bytes, sizeof(bytes), &len) != 0)
        return syntax(in, "byte string too long for", "bytes");
    if (!v) len = 0;

    g_last_status = kernel_ensure(64, 65536, 1048576, 65536);
    if (g_last_status != CDIN_OK) return 0;

    cdin_doc d = CDIN_INVALID_DOC;
    out_begin();
    g_last_status = cdin_doc_create(g_kernel, bytes, len, &d);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("create_doc", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    if (!handle_new(HK_DOC, g_kernel, d)) return syntax(in, "handle table is full", NULL);
    g_out.num_length = len;
    return 0;
}

static int op_close_doc(opin *in)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    out_begin();
    g_last_status = cdin_doc_close(g_kernel, d);
    handle_drop(in->args[0].val);
    return 0;
}

static int op_replace(opin *in)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    uint64_t start, end, expected;
    if (u64_req(in, "start", &start) != 0) return -1;
    if (u64_req(in, "end", &end) != 0) return -1;
    if (u64_req(in, "expected_version", &expected) != 0) return -1;
    int ok = 1;
    uint64_t coalesce = u64_arg(in, "coalesce_key", 0, &ok);
    if (!ok) return -1;
    uint8_t bytes[4096];
    size_t len = 0;
    if (bytes_arg(in, "bytes", bytes, sizeof(bytes), &len) != 0) return -1;

    g_out.edit.struct_size = sizeof(g_out.edit);
    out_begin();
    cdin_replace_request req;
    memset(&req, 0, sizeof(req));
    req.struct_size = sizeof(req);
    req.start = start;
    req.end = end;
    req.bytes = len ? bytes : NULL;
    req.len = len;
    req.expected_version = expected;
    req.coalesce_key = coalesce;
    g_last_status = cdin_doc_replace(g_kernel, d, &req, &g_out.edit);
    if (g_last_status != CDIN_OK && out_untouched("replace", in->ctx.path, in->ctx.line))
        (*in->ctx.errors)++;
    if (g_last_status == CDIN_OK) publish_edit();
    return 0;
}

static int op_undo_redo(opin *in, int is_undo)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    uint64_t expected;
    if (u64_req(in, "expected_version", &expected) != 0) return -1;
    cdin_history_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = expected;
    g_out.edit.struct_size = sizeof(g_out.edit);
    out_begin();
    g_last_status = is_undo ? cdin_doc_undo(g_kernel, d, &rq, &g_out.edit)
                            : cdin_doc_redo(g_kernel, d, &rq, &g_out.edit);
    if (g_last_status != CDIN_OK && out_untouched(is_undo ? "undo" : "redo",
                                                  in->ctx.path, in->ctx.line))
        (*in->ctx.errors)++;
    if (g_last_status == CDIN_OK) publish_edit();
    return 0;
}

static int op_txn_begin(opin *in)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    int ok = 1;
    uint64_t expected = u64_arg(in, "expected_version", CDIN_ANY_VERSION, &ok);
    if (!ok) return -1;
    cdin_txn_begin_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = expected;
    cdin_txn t = CDIN_INVALID_TXN;
    out_begin();
    g_last_status = cdin_txn_begin(g_kernel, d, &rq, &t);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("txn_begin", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    if (!handle_new(HK_TXN, g_kernel, t)) return syntax(in, "handle table is full", NULL);
    return 0;
}

static int op_txn_end(opin *in, int commit)
{
    const handle_entry *h;
    if (!handle_arg(in, HK_TXN, &h)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    cdin_txn_commit_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    g_out.edit.struct_size = sizeof(g_out.edit);
    out_begin();
    g_last_status = commit ? cdin_txn_commit(g_kernel, (cdin_txn)h->value, &rq, &g_out.edit)
                           : cdin_txn_abort(g_kernel, (cdin_txn)h->value);
    if (g_last_status != CDIN_OK && out_untouched(commit ? "txn_commit" : "txn_abort",
                                                  in->ctx.path, in->ctx.line))
        (*in->ctx.errors)++;
    if (g_last_status == CDIN_OK && commit) publish_edit();
    handle_drop(h->name);
    return 0;
}

static int op_replace_tx(opin *in)
{
    const handle_entry *h;
    if (!handle_arg(in, HK_TXN, &h)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    uint64_t start, end;
    if (u64_req(in, "start", &start) != 0) return -1;
    if (u64_req(in, "end", &end) != 0) return -1;
    uint8_t bytes[4096];
    size_t len = 0;
    if (bytes_arg(in, "bytes", bytes, sizeof(bytes), &len) != 0) return -1;
    cdin_replace_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.expected_version = CDIN_ANY_VERSION;
    rq.start = start;
    rq.end = end;
    rq.bytes = len ? bytes : NULL;
    rq.len = len;
    g_out.edit.struct_size = sizeof(g_out.edit);
    out_begin();
    g_last_status = cdin_txn_replace(g_kernel, (cdin_txn)h->value, &rq, &g_out.edit);
    if (g_last_status != CDIN_OK && out_untouched("replace_tx", in->ctx.path, in->ctx.line))
        (*in->ctx.errors)++;
    return 0;
}

static int op_subscribe(opin *in)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    cdin_subscribe_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.wakeup = test_wakeup;
    cdin_version cur = 0;
    cdin_notif nf = CDIN_INVALID_NOTIF;
    out_begin();
    g_last_status = cdin_notif_subscribe(g_kernel, d, &rq, &cur, &nf);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("subscribe", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    g_out.num_version = cur;
    if (!handle_new(HK_NOTIF, g_kernel, nf)) return syntax(in, "handle table is full", NULL);
    return 0;
}

static int op_unsubscribe(opin *in)
{
    const handle_entry *h;
    if (!handle_arg(in, HK_NOTIF, &h)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    out_begin();
    g_last_status = cdin_notif_unsubscribe(g_kernel, (cdin_notif)h->value);
    /* The handle stays in the table so a later drain can still find it. */
    return 0;
}

static int op_drain(opin *in)
{
    const handle_entry *h;
    if (!handle_arg(in, HK_NOTIF, &h)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    out_begin();
    g_out.event_count = 0;
    size_t count = 0;
    g_last_status = cdin_notif_drain(g_kernel, (cdin_notif)h->value, g_out.events,
                                     sizeof(g_out.events) / sizeof(g_out.events[0]), &count);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("drain", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    g_out.event_count = count;
    g_out.num_count = count;
    if (count > 0) {
        g_out.notif_kind = (uint64_t)g_out.events[0].kind;
        g_out.num_from_version = g_out.events[0].from_version;
        g_out.num_to_version = g_out.events[0].to_version;
        g_out.num_current_version = g_out.events[0].current_version;
    }
    return 0;
}

static int op_snapshot(opin *in, int what)
{
    if (what == 2) {
        const handle_entry *h;
        if (!handle_arg(in, HK_SNAP, &h)) {
            if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *in->ctx.errors ? -1 : 0;
        }
        out_begin();
        g_last_status = cdin_snapshot_release(g_kernel, (cdin_snapshot)h->value);
        handle_drop(h->name);
        return 0;
    }
    if (what == 3) {
        const handle_entry *h;
        if (!handle_arg(in, HK_SNAP, &h)) {
            if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *in->ctx.errors ? -1 : 0;
        }
        uint64_t start, end;
        if (u64_req(in, "start", &start) != 0) return -1;
        if (u64_req(in, "end", &end) != 0) return -1;
        size_t len = 0;
        out_begin();
        g_last_status = cdin_text_read(g_kernel, cdin_src_snap((cdin_snapshot)h->value), start, end,
                                       g_out.data, sizeof(g_out.data), &len, NULL);
        if (g_last_status != CDIN_OK) {
            if (out_untouched("snapshot_read", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
            return 0;
        }
        g_out.data_len = len;
        return 0;
    }
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    cdin_version cur = 0;
    cdin_snapshot s = CDIN_INVALID_SNAPSHOT;
    out_begin();
    g_last_status = cdin_snapshot_acquire(g_kernel, d, &cur, &s);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("snapshot_acquire", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    g_out.num_version = cur;
    if (!handle_new(HK_SNAP, g_kernel, s)) return syntax(in, "handle table is full", NULL);
    return 0;
}

/* Search matches land here; expect.c reads them back. The buffer lives in
 * runner_state.c (g_search_ranges). */
static int op_search(opin *in)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    uint8_t pattern[512];
    size_t pattern_len = 0;
    if (bytes_arg(in, "pattern", pattern, sizeof(pattern), &pattern_len) != 0) return -1;

    cdin_search_mode mode = CDIN_SEARCH_LITERAL;
    const char *mode_s = vec_kv(in->args, in->n, "mode", &in->ctx);
    if (mode_s && strcmp(mode_s, "bytes") == 0) mode = CDIN_SEARCH_BYTES;
    else if (mode_s && strcmp(mode_s, "regex") == 0) mode = CDIN_SEARCH_REGEX;
    else if (mode_s && strcmp(mode_s, "literal") != 0) return syntax(in, "unknown mode", mode_s);

    cdin_search_direction dir = CDIN_SEARCH_FORWARD;
    const char *dir_s = vec_kv(in->args, in->n, "direction", &in->ctx);
    if (dir_s && strcmp(dir_s, "backward") == 0) dir = CDIN_SEARCH_BACKWARD;
    else if (dir_s && strcmp(dir_s, "forward") != 0) return syntax(in, "unknown direction", dir_s);

    int ok = 1;
    uint64_t from = u64_arg(in, "from", 0, &ok);
    if (!ok) return -1;
    uint64_t limit = u64_arg(in, "limit", 8, &ok);
    if (!ok) return -1;
    if (limit == 0) limit = 16;        /* old vec syntax: no explicit limit -> fill the range array */
    uint64_t case_sensitive = u64_arg(in, "case_sensitive", 1, &ok);
    if (!ok) return -1;

    /* cancel_step and cancel are optional; u64_arg cannot tell "absent" from
     * "present with value 0", so probe each key directly. */
    uint64_t step_limit = 0;
    const uint32_t *cancel = NULL;
    uint32_t cancel_flag = 0;
    const char *cs = vec_kv(in->args, in->n, "cancel_step", &in->ctx);
    if (cs) {
        uint64_t v;
        if (vec_u64(cs, &v) != 0) return syntax(in, "invalid number for", "cancel_step");
        step_limit = v;
    }
    const char *cf = vec_kv(in->args, in->n, "cancel", &in->ctx);
    if (cf) {
        uint64_t v;
        if (vec_u64(cf, &v) != 0) return syntax(in, "invalid number for", "cancel");
        cancel_flag = (uint32_t)v;
        cancel = &cancel_flag;
    }

    cdin_search_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.struct_size = sizeof(rq);
    rq.src = cdin_src_doc(d);
    rq.expected_version = CDIN_ANY_VERSION;
    rq.pattern = pattern;
    rq.pattern_len = pattern_len;
    rq.mode = mode;
    rq.direction = dir;
    rq.from = from;
    rq.bound = UINT64_MAX;
    rq.limit = limit;
    rq.step_limit = step_limit;
    rq.cancel = cancel;
    rq.ranges = g_search_ranges;
    rq.ranges_capacity = 16;
    if (case_sensitive == 0) rq.flags = CDIN_SEARCH_CASE_INSENSITIVE;

    g_out.search.struct_size = sizeof(g_out.search);
    out_begin();
    g_last_status = cdin_search(g_kernel, &rq, &g_out.search);
    if (g_last_status != CDIN_OK) {
        if (out_untouched("search", in->ctx.path, in->ctx.line)) (*in->ctx.errors)++;
        return 0;
    }
    g_out.num_count = g_out.search.count;
    g_out.num_version = g_out.search.version;
    g_out.num_truncated = (uint64_t)g_out.search.truncated;
    return 0;
}

static int op_read(opin *in, int serialize)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    uint64_t start = 0, end = UINT64_MAX;
    if (!serialize) {
        if (u64_req(in, "start", &start) != 0) return -1;
        if (u64_req(in, "end", &end) != 0) return -1;
    }
    size_t len = 0;
    out_begin();
    g_last_status = serialize ? cdin_text_serialize(g_kernel, cdin_src_doc(d), g_out.data, sizeof(g_out.data), &len)
                              : cdin_text_read(g_kernel, cdin_src_doc(d), start, end, g_out.data,
                                               sizeof(g_out.data), &len, NULL);
    if (g_last_status != CDIN_OK) {
        if (out_untouched(serialize ? "serialize" : "read", in->ctx.path, in->ctx.line))
            (*in->ctx.errors)++;
        return 0;
    }
    g_out.data_len = len;
    return 0;
}

static int op_line(opin *in, int what)
{
    cdin_doc d;
    if (!doc_arg(in, &d, NULL)) {
        if (*in->ctx.errors == 0) g_last_status = CDIN_INVALID_HANDLE;
        return *in->ctx.errors ? -1 : 0;
    }
    out_begin();
    switch (what) {
    case 0: {
        uint64_t count = 0;
        g_last_status = cdin_text_line_count(g_kernel, cdin_src_doc(d), &count);
        if (g_last_status == CDIN_OK) g_out.num_count = count;
        break;
    }
    case 1: {
        uint64_t line;
        if (u64_req(in, "line", &line) != 0) return -1;
        uint64_t offset = 0;
        g_last_status = cdin_text_line_start(g_kernel, cdin_src_doc(d), line, &offset);
        if (g_last_status == CDIN_OK) g_out.num_offset = offset;
        break;
    }
    case 2:
    case 3: {                                  /* byte_to_line is line_from_offset (POS-013) */
        uint64_t offset;
        if (u64_req(in, "offset", &offset) != 0) return -1;
        uint64_t line = 0, col = 0;
        g_last_status = cdin_text_line_from_offset(g_kernel, cdin_src_doc(d), offset, &line, &col);
        if (g_last_status == CDIN_OK) { g_out.num_line = line; g_out.num_col = col; }
        break;
    }
    default: {                                 /* line_to_byte is line_start */
        uint64_t line;
        if (u64_req(in, "line", &line) != 0) return -1;
        uint64_t offset = 0;
        g_last_status = cdin_text_line_start(g_kernel, cdin_src_doc(d), line, &offset);
        if (g_last_status == CDIN_OK) g_out.num_offset = offset;
        break;
    }
    }
    if (g_last_status != CDIN_OK && out_untouched("line", in->ctx.path, in->ctx.line))
        (*in->ctx.errors)++;
    return 0;
}

typedef struct { const char *name; const char *keys; } op_entry;

static const op_entry OPS[] = {
    { "create", "allocator max_documents max_document_size max_undo max_blob" },
    { "destroy", "handle" },
    { "fail_alloc_after", "allocator n" },
    { "abi_version", "" },
    { "features", "" },
    { "unicode_version", "" },
    { "create_doc", "bytes" },
    { "close_doc", "handle" },
    { "version", "handle" },
    { "len", "handle" },
    { "read", "handle start end" },
    { "serialize", "handle" },
    { "replace", "handle start end bytes expected_version coalesce_key" },
    { "undo", "handle expected_version" },
    { "redo", "handle expected_version" },
    { "txn_begin", "handle expected_version" },
    { "txn_commit", "handle" },
    { "txn_abort", "handle" },
    { "replace_tx", "handle start end bytes" },
    { "mark_clean", "handle" },
    { "is_clean", "handle" },
    { "set_metadata", "handle value" },
    { "get_metadata", "handle" },
    { "subscribe", "handle" },
    { "unsubscribe", "handle" },
    { "drain", "handle" },
    { "snapshot_acquire", "handle" },
    { "snapshot_release", "handle" },
    { "snapshot_read", "handle start end" },
    { "shutdown", "mode" },
    { "search", "handle pattern mode direction from case_sensitive limit cancel_step cancel" },
    { "grapheme_next", "handle offset" },
    { "grapheme_prev", "handle offset" },
    { "byte_to_codepoint", "handle offset" },
    { "codepoint_to_byte", "handle codepoint" },
    { "check_position", "handle offset" },
    { "eol_style", "handle" },
    { "line_count", "handle" },
    { "line_start", "handle line" },
    { "line_from_offset", "handle offset" },
    { "byte_to_line", "handle offset" },
    { "line_to_byte", "handle line" }
};

static const op_entry *op_find(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++)
        if (strlen(OPS[i].name) == len && memcmp(OPS[i].name, name, len) == 0) return &OPS[i];
    return NULL;
}

int ops_execute(const char *line, const char *path, int line_num, int *errors)
{
    const char *p = line;
    while (*p && *p != ' ') p++;
    size_t op_len = (size_t)(p - line);
    const op_entry *entry = op_find(line, op_len);
    if (!entry) {
        fprintf(stderr, "%s:%d: syntax error: unknown operation '%.*s'\n",
                path, line_num, (int)op_len, line);
        (*errors)++;
        return -1;
    }

    opin in;
    memset(&in, 0, sizeof(in));
    in.op = entry->name;
    in.ctx.path = path;
    in.ctx.line = line_num;
    in.ctx.errors = errors;
    if (vec_parse_args(p, in.args, &in.n, &in.ctx) != 0) return -1;
    if (!vec_keys_allowed(in.args, in.n, entry->keys, &in.ctx)) return -1;

    /* Every block allocated from here on is attributed to this operation, so a
     * leak report names the site that has to change. */
    for (int i = 0; i < g_allocator_count; i++) alloc_set_op(g_allocators[i].ctx, entry->name);

    const char *name = entry->name;
    cdin_doc d;
    uint64_t a;

    if (strcmp(name, "create") == 0) return op_create(&in);
    if (strcmp(name, "destroy") == 0) return op_destroy(&in);
    if (strcmp(name, "create_doc") == 0) return op_create_doc(&in);
    if (strcmp(name, "close_doc") == 0) return op_close_doc(&in);
    if (strcmp(name, "replace") == 0) return op_replace(&in);
    if (strcmp(name, "undo") == 0) return op_undo_redo(&in, 1);
    if (strcmp(name, "redo") == 0) return op_undo_redo(&in, 0);
    if (strcmp(name, "txn_begin") == 0) return op_txn_begin(&in);
    if (strcmp(name, "txn_commit") == 0) return op_txn_end(&in, 1);
    if (strcmp(name, "txn_abort") == 0) return op_txn_end(&in, 0);
    if (strcmp(name, "replace_tx") == 0) return op_replace_tx(&in);
    if (strcmp(name, "subscribe") == 0) return op_subscribe(&in);
    if (strcmp(name, "unsubscribe") == 0) return op_unsubscribe(&in);
    if (strcmp(name, "drain") == 0) return op_drain(&in);
    if (strcmp(name, "snapshot_acquire") == 0) return op_snapshot(&in, 0);
    if (strcmp(name, "snapshot_release") == 0) return op_snapshot(&in, 2);
    if (strcmp(name, "snapshot_read") == 0) return op_snapshot(&in, 3);
    if (strcmp(name, "search") == 0) return op_search(&in);
    if (strcmp(name, "read") == 0) return op_read(&in, 0);
    if (strcmp(name, "serialize") == 0) return op_read(&in, 1);
    if (strncmp(name, "line_", 5) == 0 || strcmp(name, "byte_to_line") == 0)
        return op_line(&in, strcmp(name, "line_count") == 0 ? 0
                     : strcmp(name, "line_start") == 0 ? 1
                     : strcmp(name, "line_from_offset") == 0 ? 2
                     : strcmp(name, "byte_to_line") == 0 ? 3 : 4);

    /* Remaining single-purpose operations. */
    if (strcmp(name, "abi_version") == 0 || strcmp(name, "unicode_version") == 0) {
        uint32_t major = 0, minor = 0, patch = 0;
        out_begin();
        g_last_status = strcmp(name, "abi_version") == 0
                            ? cdin_abi_version(&major, &minor, &patch)
                            : cdin_unicode_version(&major, &minor, &patch);
        if (g_last_status == CDIN_OK) {
            g_out.num_abi_major = major;
            g_out.num_abi_minor = minor;
            g_out.num_abi_patch = patch;
        }
    } else if (strcmp(name, "features") == 0) {
        out_begin();
        g_last_status = cdin_features(&g_out.num_features);
    } else if (strcmp(name, "unicode_version") == 0) {
        /* handled with abi_version above */
    } else if (strcmp(name, "fail_alloc_after") == 0) {
        const char *a_name = vec_kv_req(in.args, in.n, "allocator", &in.ctx);
        if (!a_name) return -1;
        if (u64_req(&in, "n", &a) != 0) return -1;
        alloc_ctx *ae = allocator_get(a_name);
        if (!ae) return syntax(&in, "unknown allocator", a_name);
        ae->fail_after = a;
        ae->count = 0;
        g_last_status = CDIN_OK;
    } else if (strcmp(name, "eol_style") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        cdin_eol_info e;
        memset(&e, 0, sizeof e);
        e.struct_size = (uint32_t)sizeof e;
        out_begin();
        g_last_status = cdin_text_eol_info(g_kernel, cdin_src_doc(d), &e);
        if (g_last_status == CDIN_OK) {
            /* style: the dominant/only terminator, or mixed when several exist */
            uint64_t styles = (uint64_t)(e.lf > 0) + (uint64_t)(e.crlf > 0) + (uint64_t)(e.cr > 0);
            uint64_t style = styles > 1 ? 4u : (e.lf > 0 ? 1u : (e.crlf > 0 ? 2u : (e.cr > 0 ? 3u : 0u)));
            g_out.num_eol_style = style;
        }
    } else if (strcmp(name, "version") == 0 || strcmp(name, "len") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        out_begin();
        if (strcmp(name, "version") == 0) {
            cdin_version v = 0;
            g_last_status = cdin_text_version(g_kernel, cdin_src_doc(d), &v);
            if (g_last_status == CDIN_OK) g_out.num_version = v;
        } else {
            uint64_t len = 0;
            g_last_status = cdin_text_len(g_kernel, cdin_src_doc(d), &len);
            if (g_last_status == CDIN_OK) g_out.num_length = len;
        }
    } else if (strcmp(name, "mark_clean") == 0 || strcmp(name, "is_clean") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        out_begin();
        if (strcmp(name, "mark_clean") == 0) {
            g_last_status = cdin_doc_mark_clean(g_kernel, d);
        } else {
            int clean = 0;
            g_last_status = cdin_doc_is_clean(g_kernel, d, &clean);
            if (g_last_status == CDIN_OK) g_out.num_clean = (uint64_t)clean;
        }
    } else if (strcmp(name, "set_metadata") == 0 || strcmp(name, "get_metadata") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        out_begin();
        if (strcmp(name, "set_metadata") == 0) {
            if (u64_req(&in, "value", &a) != 0) return -1;
            g_last_status = cdin_doc_set_metadata(g_kernel, d, a);
        } else {
            uint64_t value = 0;
            g_last_status = cdin_doc_get_metadata(g_kernel, d, &value);
            if (g_last_status == CDIN_OK) g_out.num_offset = value;
        }
    } else if (strcmp(name, "shutdown") == 0) {
        const char *mode = vec_kv_req(in.args, in.n, "mode", &in.ctx);
        if (!mode) return -1;
        g_last_status = kernel_ensure(64, 65536, 1048576, 65536);
        if (g_last_status != CDIN_OK) return 0;
        if (strcmp(mode, "graceful") == 0) g_last_status = cdin_kernel_shutdown(g_kernel, CDIN_SHUTDOWN_GRACEFUL);
        else if (strcmp(mode, "abort") == 0) g_last_status = cdin_kernel_shutdown(g_kernel, CDIN_SHUTDOWN_ABORT);
        else return syntax(&in, "unknown shutdown mode", mode);
    } else if (strcmp(name, "grapheme_next") == 0 || strcmp(name, "grapheme_prev") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        if (u64_req(&in, "offset", &a) != 0) return -1;
        uint64_t out_off = 0;
        out_begin();
        if (strcmp(name, "grapheme_next") == 0) {
            g_last_status = cdin_text_grapheme_next(g_kernel, cdin_src_doc(d), a, &out_off);
            if (g_last_status == CDIN_OK) g_out.num_next = out_off;
        } else {
            g_last_status = cdin_text_grapheme_prev(g_kernel, cdin_src_doc(d), a, &out_off);
            if (g_last_status == CDIN_OK) g_out.num_prev = out_off;
        }
    } else if (strcmp(name, "byte_to_codepoint") == 0 || strcmp(name, "codepoint_to_byte") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        out_begin();
        if (strcmp(name, "byte_to_codepoint") == 0) {
            if (u64_req(&in, "offset", &a) != 0) return -1;
            uint64_t cp = 0;
            g_last_status = cdin_text_byte_to_codepoint(g_kernel, cdin_src_doc(d), a, &cp, NULL);
            if (g_last_status == CDIN_OK) g_out.num_codepoint = cp;
        } else {
            if (u64_req(&in, "codepoint", &a) != 0) return -1;
            uint64_t off = 0;
            g_last_status = cdin_text_codepoint_to_byte(g_kernel, cdin_src_doc(d), a, &off, NULL);
            if (g_last_status == CDIN_OK) g_out.num_offset = off;
        }
    } else if (strcmp(name, "check_position") == 0) {
        if (!doc_arg(&in, &d, NULL)) {
            if (*errors == 0) g_last_status = CDIN_INVALID_HANDLE;
            return *errors ? -1 : 0;
        }
        if (u64_req(&in, "offset", &a) != 0) return -1;
        out_begin();
        g_last_status = cdin_text_check_position(g_kernel, cdin_src_doc(d), CDIN_ANY_VERSION, a);
        g_out.num_valid = (g_last_status == CDIN_OK);
    } else {
        fprintf(stderr, "%s:%d: internal error: operation '%s' has no implementation\n",
                path, line_num, name);
        (*errors)++;
        return -1;
    }
    return 0;
}
