/* Transactions (TXN-*): a private tentative copy of the document that is
 * published atomically.
 *
 * The tentative text shares the document's block (copy-on-write) until the
 * first edit, so beginning a transaction costs O(1) and the history stays
 * proportional to the edits (TXN-005). While a transaction is active every
 * other mutation of the document fails with BUSY (TXN-004), so the document
 * cannot change underneath it. Commit publishes by swapping text blocks:
 * nothing after the last fallible step can fail. */
#include "internal.h"

static cdin_status txn_lookup(cdin_kernel *k, cdin_txn th, txn_state **tx, doc **d)
{
    txn_state *t;
    doc *dd;
    if (k->stopped) return CDIN_SHUTDOWN;
    t = pool_get(&k->txns, th);
    if (!t) return CDIN_INVALID_HANDLE;
    dd = pool_get(&k->docs, t->doc);
    if (!dd || dd->tx != t) return CDIN_INTERNAL;
    *tx = t;
    *d = dd;
    return CDIN_OK;
}

/* Resolves a handle for the api layer and leaves both pointers set. */
cdin_status txn_resolve(cdin_kernel *k, cdin_txn th, txn_state **tx, doc **d)
{ return txn_lookup(k, th, tx, d); }

cdin_status txn_begin_locked(cdin_kernel *k, doc *d, const cdin_txn_begin_request *req,
                             cdin_txn *out)
{
    txn_state *tx;
    cdin_status st;
    cdin_edit_result tmp;
    st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    if (d->tx) return CDIN_INVALID_STATE;      /* TXN-020: no nesting */
    memset(&tmp, 0, sizeof tmp);
    st = accept_check(k, d, req->expected_version, &tmp);
    if (st != CDIN_OK) return st;
    tx = k_alloc(k, sizeof *tx);
    if (!tx) return CDIN_NO_MEMORY;
    memset(tx, 0, sizeof *tx);
    tx->doc = d->handle;
    tx->work = d->txt;
    tx->serial = ++k->txn_serial;
    st = pool_insert(k, &k->txns, tx, &tx->handle);
    if (st != CDIN_OK) { k_free(k, tx, sizeof *tx); return st; }
    d->txt->refs++;
    d->tx = tx;
    *out = tx->handle;
    return CDIN_OK;
}

static cdin_status tx_edits_reserve(cdin_kernel *k, txn_state *tx)
{
    if (tx->n == tx->cap) {
        size_t ncap = tx->cap ? tx->cap * 2 : 8, ncap_bytes;
        edit_rec *ne;
        if (k_mul(ncap, sizeof(edit_rec), &ncap_bytes) == 0) return CDIN_LIMIT_EXCEEDED;
        ne = tx->edits ? k_realloc(k, tx->edits, tx->cap * sizeof(edit_rec), ncap_bytes)
                       : k_alloc(k, ncap_bytes);
        if (!ne) return CDIN_NO_MEMORY;
        tx->edits = ne;
        tx->cap = ncap;
    }
    return CDIN_OK;
}

cdin_status txn_replace_locked(cdin_kernel *k, txn_state *tx, doc *d,
                               const cdin_replace_request *req, cdin_edit_result *out)
{
    cdin_status st;
    edit_rec rec;
    uint64_t newlen;
    details_clear(out);
    if (!req) return CDIN_INVALID_ARGUMENT;
    st = txn_replace_validate(k, tx->work, req, out);
    if (st != CDIN_OK) return st;
    if (req->start == req->end && req->len == 0) {
        out->new_version = d->version; out->edit_count = 0;
        out->start = out->removed = out->inserted = 0;
        out->new_length = tx->work->len; out->blob_len = 0;
        return CDIN_OK;
    }
    if ((uint64_t)tx->bytes + (req->end - req->start) + req->len + sizeof(edit_rec) > k->max_undo_memory) {
        details_limit(out, CDIN_LIMIT_TXN_MEMORY);
        return CDIN_LIMIT_EXCEEDED;
    }
    st = tx_edits_reserve(k, tx);
    if (st != CDIN_OK) return st;
    st = edit_rec_make(k, tx->work, req->start, req->end, req->bytes, req->len, &rec);
    if (st != CDIN_OK) return st;
    newlen = tx->work->len - (req->end - req->start) + req->len;
    st = text_reserve(k, &tx->work, (size_t)(newlen > tx->work->len ? newlen : tx->work->len));
    if (st != CDIN_OK) { edits_free(k, &rec, 1); return st; }
    text_splice(k, tx->work, req->start, req->end - req->start, req->bytes, req->len);
    tx->edits[tx->n++] = rec;
    tx->bytes += edit_bytes(&rec) + sizeof(edit_rec);
    out->new_version = d->version;
    out->edit_count = 1;
    out->start = rec.start; out->removed = rec.removed_len; out->inserted = rec.inserted_len;
    out->new_length = tx->work->len;
    out->blob_len = 0;
    return CDIN_OK;
}

cdin_status txn_commit_locked(cdin_kernel *k, txn_state *tx, doc *d,
                              const cdin_txn_commit_request *req, cdin_edit_result *out,
                              wake_list *wl)
{
    cdin_status st;
    blobs b;
    commit_prep cp;
    edit_rec *fin = NULL;
    text *old;
    st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    b = (blobs){ req->blob_before, req->blob_before_len, req->blob_after, req->blob_after_len };
    st = blobs_check(k, &b, out);
    if (st != CDIN_OK) return st;
    if (tx->n == 0) {                                      /* empty transaction: nothing to publish */
        pool_remove(&k->txns, tx->handle);
        d->tx = NULL;
        txn_free(k, tx);
        out->new_version = d->version; out->edit_count = 0;
        out->start = out->removed = out->inserted = 0;
        out->new_length = d->txt->len; out->blob_len = 0;
        return CDIN_OK;
    }
    st = accept_check(k, d, CDIN_ANY_VERSION, out);        /* version cannot have moved: BUSY guards it */
    if (st != CDIN_OK) return st;
    fin = k_alloc(k, tx->n * sizeof(edit_rec));
    if (fin) {
        memcpy(fin, tx->edits, tx->n * sizeof(edit_rec));
        st = commit_prepare(k, d, CK_TXN, fin, tx->n, 0, &b, &cp);
        if (st != CDIN_OK) k_free(k, fin, tx->n * sizeof(edit_rec));
    } else {
        st = CDIN_NO_MEMORY;
    }
    if (st != CDIN_OK) {                       /* TXN-016: a failed commit rolls the transaction back completely */
        pool_remove(&k->txns, tx->handle);
        d->tx = NULL;
        txn_free(k, tx);
        return st;
    }

    /* ---- infallible from here ---- */
    old = d->txt;
    d->txt = tx->work;                                      /* the document takes the transaction's block */
    tx->work = NULL;
    text_release(k, old);
    d->tx = NULL;
    pool_remove(&k->txns, tx->handle);
    {
        size_t n = tx->n;
        k_free(k, tx->edits, tx->cap * sizeof(edit_rec));   /* buffers now belong to `fin` */
        tx->edits = NULL; tx->n = tx->cap = 0;
        k_free(k, tx, sizeof *tx);
        commit_finish(k, d, CK_TXN, &cp, fin, n, 0, wl, out);
    }
    return CDIN_OK;
}

cdin_status txn_abort_locked(cdin_kernel *k, txn_state *tx, doc *d)
{
    pool_remove(&k->txns, tx->handle);
    d->tx = NULL;
    txn_free(k, tx);
    return CDIN_OK;
}

cdin_status txn_len_locked(cdin_kernel *k, txn_state *tx, doc *d, uint64_t *out)
{
    (void)k; (void)d;
    if (!out) return CDIN_INVALID_ARGUMENT;
    *out = tx->work->len;
    return CDIN_OK;
}

cdin_status txn_read_locked(cdin_kernel *k, txn_state *tx, doc *d, uint64_t start, uint64_t end,
                            uint8_t *buf, size_t bufsz, size_t *out_len)
{
    size_t n;
    (void)k; (void)d;
    if (!out_len) return CDIN_INVALID_ARGUMENT;
    if (start > end || end > tx->work->len) return CDIN_OUT_OF_RANGE;
    n = (size_t)(end - start);
    *out_len = n;
    if (buf) {
        if (bufsz < n) return CDIN_BUFFER_TOO_SMALL;
        if (n) memcpy(buf, tx->work->data + start, n);
    }
    return CDIN_OK;
}
