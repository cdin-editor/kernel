/* Document table and the document lifecycle: create, close, reload, saved
 * state, metadata, history counters, snapshots.
 *
 * The table is one handle pool (core/handle_table.c); doc_lookup is the single
 * place that rejects a stale handle or a stopped kernel. Close and reload are
 * the two operations that replace or free the block of a document, so both
 * must run under the lock and emit their events after it is released; that is
 * why the *_locked functions here receive the caller's wake list. */
#include "internal.h"

cdin_status doc_lookup(cdin_kernel *k, cdin_doc d, doc **out)
{
    doc *p;
    p = pool_get(&k->docs, d);
    if (!p) return CDIN_INVALID_HANDLE;
    *out = p;
    return CDIN_OK;
}

cdin_status doc_lookup_mut(cdin_kernel *k, cdin_doc d, doc **out)
{
    if (k->stopped) return CDIN_SHUTDOWN;       /* CONC-020: no acceptance after shutdown */
    return doc_lookup(k, d, out);
}

/* Lock + resolve a read-only source (document or snapshot) for the text/api
 * layer. Caller must k_unlock on error and call k_unlock after the body. */
cdin_status text_qbegin(cdin_kernel *k, cdin_source src, text **t, cdin_version *v)
{
    cdin_status st;
    k_lock(k);
    st = src_lookup(k, src, t, v);
    if (st != CDIN_OK) k_unlock(k);
    return st;
}

cdin_status src_lookup(cdin_kernel *k, cdin_source src, text **t, cdin_version *v)
{
    if ((src.doc != 0) == (src.snapshot != 0)) return CDIN_INVALID_ARGUMENT;
    if (src.doc) {
        doc *d;
        cdin_status st = doc_lookup(k, src.doc, &d);
        if (st != CDIN_OK) return st;
        *t = d->txt;
        *v = d->version;
        return CDIN_OK;
    } else {
        snapshot *s = pool_get(&k->snaps, src.snapshot);
        if (!s) return CDIN_INVALID_HANDLE;
        *t = s->txt;
        *v = s->version;
        return CDIN_OK;
    }
}

/* ---- frees (internal) ------------------------------------------------------------ */
void txn_free(cdin_kernel *k, txn_state *tx)
{
    if (!tx) return;
    edits_free(k, tx->edits, tx->n);
    k_free(k, tx->edits, tx->cap * sizeof(edit_rec));
    text_release(k, tx->work);
    k_free(k, tx, sizeof *tx);
}

void doc_free_contents(cdin_kernel *k, doc *d)
{
    for (size_t i = 0; i < d->nunits; i++) unit_free(k, &d->units[i]);
    k_free(k, d->units, d->units_cap * sizeof(unit));
    text_release(k, d->txt);
    if (d->tx) {
        pool_remove(&k->txns, d->tx->handle);
        txn_free(k, d->tx);
        d->tx = NULL;
    }
    d->units = NULL; d->nunits = d->units_cap = 0; d->txt = NULL;
}

/* Emits the final `closed` event, detaches subscriptions, removes and frees the document. */
void doc_close_internal(cdin_kernel *k, doc *d, wake_list *wl)
{
    cdin_doc h = d->handle;
    emit_simple(k, d, CDIN_NOTIF_CLOSED, 0, 0, wl);
    subs_detach_doc(k, d);
    doc_free_contents(k, d);
    pool_remove(&k->docs, h);
    k_free(k, d, sizeof *d);
}

/* ---- create / close / reload ------------------------------------------------------ */
cdin_status doc_create_locked(cdin_kernel *k, const uint8_t *bytes, size_t len, cdin_doc *out)
{
    cdin_status st;
    doc *d;
    text *t;
    if (k->stopped) return CDIN_SHUTDOWN;
    if (k->docs.live >= k->max_documents || len > k->max_document_size) return CDIN_LIMIT_EXCEEDED;
    if (len && !bytes) return CDIN_INVALID_ARGUMENT;
    t = text_new(k, len);
    if (!t) return CDIN_NO_MEMORY;
    if (len) memcpy(t->data, bytes, len);
    t->len = len;
    d = k_alloc(k, sizeof *d);
    if (!d) { text_release(k, t); return CDIN_NO_MEMORY; }
    memset(d, 0, sizeof *d);
    d->txt = t;
    st = pool_insert(k, &k->docs, d, &d->handle);
    if (st != CDIN_OK) { text_release(k, t); k_free(k, d, sizeof *d); return st; }
    *out = d->handle;
    return CDIN_OK;
}

cdin_status doc_close_locked(cdin_kernel *k, doc *d, wake_list *wl)
{
    doc_close_internal(k, d, wl);
    return CDIN_OK;
}

cdin_status doc_reload_locked(cdin_kernel *k, doc *d, const cdin_reload_request *req,
                              cdin_edit_result *out, wake_list *wl)
{
    cdin_status st;
    text *t;
    int was_clean;
    cdin_version from;
    st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    if (req->len && !req->bytes) return CDIN_INVALID_ARGUMENT;
    if (d->tx) return CDIN_BUSY;
    if (req->len > k->max_document_size) { details_limit(out, CDIN_LIMIT_DOCUMENT_SIZE); return CDIN_LIMIT_EXCEEDED; }
    st = accept_check(k, d, req->expected_version, out);
    if (st != CDIN_OK) return st;
    t = text_new(k, req->len);
    if (!t) return CDIN_NO_MEMORY;
    if (req->len) memcpy(t->data, req->bytes, req->len);
    t->len = req->len;
    was_clean = doc_is_clean(d);
    from = d->version;
    text_release(k, d->txt);                 /* snapshots keep the old block alive */
    d->txt = t;
    for (size_t i = 0; i < d->nunits; i++) unit_free(k, &d->units[i]);
    d->nunits = d->head = 0;
    d->undo_bytes = 0;
    d->marker_id = 0;
    d->marker_lost = 0;                      /* the reloaded content is the saved content */
    d->coalesce_unit = 0;
    d->version++;
    out->new_version = d->version;
    out->edit_count = 0;
    out->start = out->removed = out->inserted = 0;
    out->new_length = t->len;
    out->blob_len = 0;
    emit_doc_event(k, d, CDIN_NOTIF_RELOAD, from, d->version, NULL, 0, 0, wl);
    if (!was_clean) emit_simple(k, d, CDIN_NOTIF_SAVED_STATE_CHANGED, 0, 1, wl);
    return CDIN_OK;
}

/* ---- saved state, metadata, history info ------------------------------------------- */
cdin_status doc_mark_clean_locked(cdin_kernel *k, doc *d, wake_list *wl)
{
    int was = doc_is_clean(d);
    if (k->seq == UINT64_MAX) return CDIN_LIMIT_EXCEEDED;
    k->seq++;
    d->marker_id = d->head ? d->units[d->head - 1].id : 0;
    d->marker_lost = 0;
    d->coalesce_unit = 0;                /* never merge new edits into the saved state */
    if (!was) emit_simple(k, d, CDIN_NOTIF_SAVED_STATE_CHANGED, 0, 1, wl);
    return CDIN_OK;
}

cdin_status doc_history_info_locked(cdin_kernel *k, doc *d, uint64_t *undo_units,
                                    uint64_t *redo_units, uint64_t *evicted_total,
                                    uint64_t *memory_bytes)
{
    (void)k;
    if (undo_units) *undo_units = d->head;
    if (redo_units) *redo_units = d->nunits - d->head;
    if (evicted_total) *evicted_total = d->evicted_total;
    if (memory_bytes) *memory_bytes = d->undo_bytes;
    return CDIN_OK;
}

/* ---- snapshots (CONC-005/006) ------------------------------------------------------- */
cdin_status snapshot_acquire_locked(cdin_kernel *k, doc *d, cdin_version *out_version,
                                    cdin_snapshot *out)
{
    snapshot *s;
    cdin_status st;
    if (d->nsnaps >= k->max_snapshots) return CDIN_LIMIT_EXCEEDED;
    s = k_alloc(k, sizeof *s);
    if (!s) return CDIN_NO_MEMORY;
    s->txt = d->txt;
    s->version = d->version;
    s->doc = d->handle;
    st = pool_insert(k, &k->snaps, s, out);
    if (st != CDIN_OK) { k_free(k, s, sizeof *s); return st; }
    d->txt->refs++;
    d->nsnaps++;
    if (out_version) *out_version = s->version;
    return CDIN_OK;
}

cdin_status snapshot_release_locked(cdin_kernel *k, cdin_snapshot sh)
{
    snapshot *s;
    s = pool_remove(&k->snaps, sh);
    if (!s) return CDIN_INVALID_HANDLE;
    {
        doc *d = pool_get(&k->docs, s->doc);
        if (d && d->nsnaps) d->nsnaps--;
    }
    text_release(k, s->txt);
    k_free(k, s, sizeof *s);
    return CDIN_OK;
}
