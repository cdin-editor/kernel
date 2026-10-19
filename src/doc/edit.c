/* The only content-mutation paths of a document: replace, batch, undo, redo,
 * and the commit machinery shared with transactions (txn.c).
 *
 * Atomicity rule (EDIT-010, ERR-002, ERR-005): every mutation is
 *   1. validated (no state touched),
 *   2. accepted (sequence number), version-checked,
 *   3. prepared: every allocation that can fail is made (commit_prepare, text_reserve),
 *   4. applied: memmove/memcpy and bookkeeping only, which cannot fail.
 * A failure in 1..3 frees what was allocated and leaves document and version unchanged.
 *
 * Not here: the bookkeeping of the unit array (cap, eviction, clean marker)
 * lives in history/undo.c; notifications are built in notif/notif.c; the txn
 * lifecycle lives in txn/txn.c. */
#include "internal.h"

void details_clear(cdin_edit_result *out)
{
    if (!out) return;
    memset(&out->details, 0, sizeof out->details);
    out->details.struct_size = (uint32_t)sizeof out->details;
}

void details_limit(cdin_edit_result *out, uint32_t limit_id)
{
    if (!out) return;
    details_clear(out);
    out->details.limit_id = limit_id;
}

cdin_status out_check(const cdin_edit_result *out)
{
    if (!out) return CDIN_INVALID_ARGUMENT;
    if (out->struct_size < sizeof *out) return CDIN_INVALID_ARGUMENT;
    return CDIN_OK;
}

cdin_status blobs_check(cdin_kernel *k, const blobs *b, cdin_edit_result *out)
{
    if (!b) return CDIN_OK;
    if ((b->before_len && !b->before) || (b->after_len && !b->after)) return CDIN_INVALID_ARGUMENT;
    if (b->before_len > k->max_blob_size || b->after_len > k->max_blob_size) {
        details_limit(out, CDIN_LIMIT_BLOB_SIZE);
        return CDIN_LIMIT_EXCEEDED;
    }
    return CDIN_OK;
}

/* Step 2 of the atomicity rule: the atomic version check, then acceptance.
 * CONC-004: a request that fails before acceptance has no sequence number and
 * does not take part in the order, so the version check comes first. */
cdin_status accept_check(cdin_kernel *k, doc *d, cdin_version expected, cdin_edit_result *out)
{
    if (expected != CDIN_ANY_VERSION && expected != d->version) {
        details_clear(out);
        out->details.expected = expected;
        out->details.current = d->version;
        return CDIN_STALE_VERSION;
    }
    out->accepted_seq = ++k->seq;
    return CDIN_OK;
}

/* ---- commit ---------------------------------------------------------------------- */
cdin_status commit_prepare(cdin_kernel *k, doc *d, commit_kind kind, const edit_rec *edits, size_t n,
                           uint64_t key, const blobs *b, commit_prep *cp)
{
    size_t bytes = n * sizeof(edit_rec);
    size_t bb = b ? b->before_len : 0, ba = b ? b->after_len : 0;
    int co;
    memset(cp, 0, sizeof *cp);
    for (size_t i = 0; i < n; i++) bytes += edit_bytes(&edits[i]);
    co = kind == CK_EDIT && key != 0 && d->coalesce_unit != 0 && d->head == d->nunits && d->nunits > 0 &&
         d->units[d->nunits - 1].id == d->coalesce_unit && d->units[d->nunits - 1].key == key;
    if (co) {
        unit *u = &d->units[d->nunits - 1];
        size_t want_bytes;
        if (u->n + n > u->cap) {
            size_t ncap = u->cap * 2;
            edit_rec *ne;
            if (ncap < u->n + n) ncap = u->n + n;
            if (k_mul(ncap, sizeof(edit_rec), &want_bytes) == 0) return CDIN_LIMIT_EXCEEDED;
            ne = k_realloc(k, u->edits, u->cap * sizeof(edit_rec), want_bytes);
            if (!ne) return CDIN_NO_MEMORY;
            u->edits = ne;
            u->cap = ncap;
        }
        if (ba) {
            cp->blob_after = k_dup(k, b->after, ba);
            if (!cp->blob_after) return CDIN_NO_MEMORY;
            cp->blob_after_len = ba;
        }
        cp->coalesce = 1;
        cp->delta_bytes = bytes + ba;
        return CDIN_OK;
    }
    if (bb) {
        cp->blob_before = k_dup(k, b->before, bb);
        if (!cp->blob_before) return CDIN_NO_MEMORY;
        cp->blob_before_len = bb;
    }
    if (ba) {
        cp->blob_after = k_dup(k, b->after, ba);
        if (!cp->blob_after) { commit_abort(k, cp); return CDIN_NO_MEMORY; }
        cp->blob_after_len = ba;
    }
    if (units_reserve(k, d, d->head + 1) != CDIN_OK) { commit_abort(k, cp); return CDIN_NO_MEMORY; }
    cp->delta_bytes = bytes + bb + ba;
    return CDIN_OK;
}

void commit_abort(cdin_kernel *k, commit_prep *cp)
{
    k_free(k, cp->blob_before, cp->blob_before_len);
    k_free(k, cp->blob_after, cp->blob_after_len);
    memset(cp, 0, sizeof *cp);
}

void commit_finish(cdin_kernel *k, doc *d, commit_kind kind, commit_prep *cp, edit_rec *edits,
                   size_t n, uint64_t key, wake_list *wl, cdin_edit_result *out)
{
    cdin_version from = d->version;
    uint64_t evicted = 0;
    const edit_rec *described;
    unit *u;
    d->version++;
    if (cp->coalesce) {
        size_t old_after;
        u = &d->units[d->nunits - 1];
        memcpy(u->edits + u->n, edits, n * sizeof(edit_rec));
        described = u->edits + u->n;
        u->n += n;
        old_after = u->blob_after_len;
        k_free(k, u->blob_after, old_after);
        u->blob_after = cp->blob_after;
        u->blob_after_len = cp->blob_after_len;
        u->bytes = u->bytes + cp->delta_bytes - old_after;
        d->undo_bytes = d->undo_bytes + cp->delta_bytes - old_after;
        k_free(k, edits, n * sizeof(edit_rec));              /* buffers moved into the unit; free the shell */
    } else {
        units_drop_redo(k, d);                                /* UNDO-003: discard redo units */
        u = &d->units[d->nunits++];
        memset(u, 0, sizeof *u);
        u->id = ++d->next_unit_id;
        u->edits = edits;
        u->n = n;
        u->cap = n;
        u->key = key;
        u->blob_before = cp->blob_before; u->blob_before_len = cp->blob_before_len;
        u->blob_after = cp->blob_after;   u->blob_after_len = cp->blob_after_len;
        u->bytes = cp->delta_bytes;
        d->undo_bytes += cp->delta_bytes;
        d->coalesce_unit = (kind == CK_EDIT && key != 0) ? u->id : 0;
        described = u->edits;
    }
    d->head = d->nunits;

    out->new_version = d->version;
    out->edit_count = (uint32_t)n;
    if (n == 1) {
        out->start = described[0].start;
        out->removed = described[0].removed_len;
        out->inserted = described[0].inserted_len;
    } else {
        out->start = out->removed = out->inserted = 0;
    }
    out->new_length = d->txt->len;
    out->blob_len = 0;
    /* The event must be built before eviction moves the units around. */
    emit_doc_event(k, d, kind == CK_EDIT ? CDIN_NOTIF_EDIT : kind == CK_BATCH ? CDIN_NOTIF_BATCH
                                                                              : CDIN_NOTIF_TRANSACTION,
                   from, d->version, described, n, 0, wl);

    while (d->undo_bytes > k->max_undo_memory && d->nunits > 1) {   /* UNDO-030/031: never the newest */
        evict_oldest(k, d);
        evicted++;
    }
    d->evicted_total += evicted;
    if (evicted) emit_simple(k, d, CDIN_NOTIF_HISTORY_EVICTED, evicted, 0, wl);
    /* No SAVED_STATE_CHANGED here: the recorded contract emits it only from
     * mark_clean and reload (dirty -> clean), not when an edit dirties the doc
     * (notif.basic.vec counts 3 events for edit/edit/undo). */
    memset(cp, 0, sizeof *cp);
}

/* ---- replace validation ------------------------------------------------------------ */
static cdin_status replace_validate(cdin_kernel *k, const text *t, const cdin_replace_request *req,
                                    cdin_edit_result *out, int in_txn)
{
    uint64_t newlen;
    cdin_status st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    if (req->len && !req->bytes) return CDIN_INVALID_ARGUMENT;
    if (req->start > req->end) return CDIN_INVALID_ARGUMENT;
    if (in_txn && (req->expected_version != CDIN_ANY_VERSION || req->coalesce_key != 0 ||
                   req->blob_before_len || req->blob_after_len))
        return CDIN_INVALID_ARGUMENT;
    if (req->end > t->len) {
        details_clear(out);
        out->details.requested = req->end;
        out->details.length = t->len;
        return CDIN_OUT_OF_RANGE;
    }
    st = text_pos_check(t, req->start);
    if (st != CDIN_OK) {
        details_clear(out);
        out->details.requested = req->start;
        out->details.length = t->len;
        return st;
    }
    st = text_pos_check(t, req->end);
    if (st != CDIN_OK) {
        details_clear(out);
        out->details.requested = req->end;
        out->details.length = t->len;
        return st;
    }
    if (req->len > k->max_edit_size) { details_limit(out, CDIN_LIMIT_EDIT_SIZE); return CDIN_LIMIT_EXCEEDED; }
    newlen = t->len - (req->end - req->start);
    if (req->len > k->max_document_size - (newlen > k->max_document_size ? k->max_document_size : newlen) ||
        newlen > k->max_document_size) {
        details_limit(out, CDIN_LIMIT_DOCUMENT_SIZE);
        return CDIN_LIMIT_EXCEEDED;
    }
    return CDIN_OK;
}

cdin_status txn_replace_validate(cdin_kernel *k, const text *t, const cdin_replace_request *req,
                                 cdin_edit_result *out)
{
    return replace_validate(k, t, req, out, 1);
}

/* ---- replace ----------------------------------------------------------------------- */
cdin_status doc_replace_locked(cdin_kernel *k, cdin_doc dh, const cdin_replace_request *req,
                                      cdin_edit_result *out, wake_list *wl)
{
    doc *d;
    cdin_status st;
    edit_rec *e;
    commit_prep cp;
    blobs b;
    uint64_t newlen;
    st = out_check(out);
    if (st != CDIN_OK) return st;
    st = doc_lookup_mut(k, dh, &d);
    if (st != CDIN_OK) return st;
    details_clear(out);
    if (!req) return CDIN_INVALID_ARGUMENT;
    if (d->tx) return CDIN_BUSY;
    st = replace_validate(k, d->txt, req, out, 0);
    if (st != CDIN_OK) return st;
    b = (blobs){ req->blob_before, req->blob_before_len, req->blob_after, req->blob_after_len };
    st = blobs_check(k, &b, out);
    if (st != CDIN_OK) return st;
    if (req->start == req->end && req->len == 0) {            /* DOC-012: no-op */
        out->new_version = d->version;
        out->edit_count = 0;
        out->start = out->removed = out->inserted = 0;
        out->new_length = d->txt->len;
        out->blob_len = 0;
        return CDIN_OK;
    }
    st = accept_check(k, d, req->expected_version, out);
    if (st != CDIN_OK) return st;

    e = k_alloc(k, sizeof *e);
    if (!e) return CDIN_NO_MEMORY;
    st = edit_rec_make(k, d->txt, req->start, req->end, req->bytes, req->len, e);
    if (st != CDIN_OK) { k_free(k, e, sizeof *e); return st; }
    st = commit_prepare(k, d, CK_EDIT, e, 1, req->coalesce_key, &b, &cp);
    if (st != CDIN_OK) { edits_free(k, e, 1); k_free(k, e, sizeof *e); return st; }
    newlen = d->txt->len - (req->end - req->start) + req->len;
    st = text_reserve(k, &d->txt, (size_t)(newlen > d->txt->len ? newlen : d->txt->len));
    if (st != CDIN_OK) {
        commit_abort(k, &cp);
        edits_free(k, e, 1);
        k_free(k, e, sizeof *e);
        return st;
    }
    text_splice(k, d->txt, req->start, req->end - req->start, req->bytes, req->len);
    commit_finish(k, d, CK_EDIT, &cp, e, 1, req->coalesce_key, wl, out);
    return CDIN_OK;
}

/* ---- batch -------------------------------------------------------------------------- */
cdin_status doc_batch_locked(cdin_kernel *k, cdin_doc dh, const cdin_batch_request *req,
                             cdin_edit_result *out, wake_list *wl)
{
    doc *d;
    cdin_status st;
    edit_rec *e = NULL;
    commit_prep cp;
    blobs b;
    size_t i, n;
    uint64_t total_ins = 0, cur, maxlen, prev_end = 0;
    int noop = 1;
    st = out_check(out);
    if (st != CDIN_OK) return st;
    st = doc_lookup_mut(k, dh, &d);
    if (st != CDIN_OK) return st;
    details_clear(out);
    st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    if (req->coalesce_key != 0) return CDIN_INVALID_ARGUMENT;
    if (d->tx) return CDIN_BUSY;
    n = req->count;
    if (n > 0 && (!req->starts || !req->ends || !req->bytes || !req->lens)) return CDIN_INVALID_ARGUMENT;
    cur = d->txt->len;
    for (i = 0; i < n; i++) {
        uint64_t s = req->starts[i], en = req->ends[i];
        if (s > en || s < prev_end) return CDIN_INVALID_ARGUMENT;      /* unordered or overlapping */
        if (en > d->txt->len) {
            out->details.requested = en; out->details.length = d->txt->len;
            return CDIN_OUT_OF_RANGE;
        }
        if (req->lens[i] && !req->bytes[i]) return CDIN_INVALID_ARGUMENT;
        st = text_pos_check(d->txt, s);
        if (st == CDIN_OK) st = text_pos_check(d->txt, en);
        if (st != CDIN_OK) { out->details.requested = s; out->details.length = d->txt->len; return st; }
        if (req->lens[i] > k->max_edit_size) { details_limit(out, CDIN_LIMIT_EDIT_SIZE); return CDIN_LIMIT_EXCEEDED; }
        total_ins += req->lens[i];
        if (total_ins > k->max_edit_size) { details_limit(out, CDIN_LIMIT_EDIT_SIZE); return CDIN_LIMIT_EXCEEDED; }
        if (en > s || req->lens[i]) noop = 0;
        prev_end = en;
    }
    /* document size after the batch, and the largest intermediate length when applied back to front */
    maxlen = cur;
    for (i = n; i-- > 0;) {
        cur = cur - (req->ends[i] - req->starts[i]) + req->lens[i];
        if (cur > k->max_document_size) { details_limit(out, CDIN_LIMIT_DOCUMENT_SIZE); return CDIN_LIMIT_EXCEEDED; }
        if (cur > maxlen) maxlen = cur;
    }
    b = (blobs){ req->blob_before, req->blob_before_len, req->blob_after, req->blob_after_len };
    st = blobs_check(k, &b, out);
    if (st != CDIN_OK) return st;
    if (n == 0 || noop) {
        out->new_version = d->version; out->edit_count = 0;
        out->start = out->removed = out->inserted = 0;
        out->new_length = d->txt->len; out->blob_len = 0;
        return CDIN_OK;
    }
    st = accept_check(k, d, req->expected_version, out);
    if (st != CDIN_OK) return st;

    e = k_alloc(k, n * sizeof *e);
    if (!e) return CDIN_NO_MEMORY;
    for (i = 0; i < n; i++) {
        /* applied back to front, so each start is in the coordinates just before its edit */
        size_t j = n - 1 - i;
        st = edit_rec_make(k, d->txt, req->starts[j], req->ends[j], req->bytes[j], req->lens[j], &e[i]);
        if (st != CDIN_OK) { edits_free(k, e, i); k_free(k, e, n * sizeof *e); return st; }
    }
    st = commit_prepare(k, d, CK_BATCH, e, n, 0, &b, &cp);
    if (st != CDIN_OK) { edits_free(k, e, n); k_free(k, e, n * sizeof *e); return st; }
    st = text_reserve(k, &d->txt, (size_t)maxlen);
    if (st != CDIN_OK) {
        commit_abort(k, &cp);
        edits_free(k, e, n); k_free(k, e, n * sizeof *e);
        return st;
    }
    for (i = n; i-- > 0;)
        text_splice(k, d->txt, req->starts[i], req->ends[i] - req->starts[i], req->bytes[i], req->lens[i]);
    commit_finish(k, d, CK_BATCH, &cp, e, n, 0, wl, out);
    return CDIN_OK;
}

/* ---- undo / redo --------------------------------------------------------------------- */
cdin_status history_locked(cdin_kernel *k, cdin_doc dh, const cdin_history_request *req,
                           cdin_edit_result *out, int undo, wake_list *wl)
{
    doc *d;
    cdin_status st;
    unit *u;
    const uint8_t *blob;
    size_t blob_len, i;
    uint64_t cur, maxlen;
    cdin_version from;
    st = out_check(out);
    if (st != CDIN_OK) return st;
    st = doc_lookup_mut(k, dh, &d);
    if (st != CDIN_OK) return st;
    details_clear(out);
    st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    if (d->tx) return CDIN_BUSY;
    if (undo ? d->head == 0 : d->head == d->nunits) return CDIN_INVALID_STATE;
    u = &d->units[undo ? d->head - 1 : d->head];
    blob = undo ? u->blob_before : u->blob_after;
    blob_len = undo ? u->blob_before_len : u->blob_after_len;
    if (req->blob_out && req->blob_capacity < blob_len) {
        out->details.required = blob_len;
        return CDIN_BUFFER_TOO_SMALL;
    }
    st = accept_check(k, d, req->expected_version, out);
    if (st != CDIN_OK) return st;

    cur = maxlen = d->txt->len;
    for (i = 0; i < u->n; i++) {
        const edit_rec *e = &u->edits[undo ? u->n - 1 - i : i];
        cur = cur - (undo ? e->inserted_len : e->removed_len) + (undo ? e->removed_len : e->inserted_len);
        if (cur > maxlen) maxlen = cur;
    }
    st = text_reserve(k, &d->txt, (size_t)maxlen);
    if (st != CDIN_OK) return st;

    for (i = 0; i < u->n; i++) {
        const edit_rec *e = &u->edits[undo ? u->n - 1 - i : i];
        if (undo) text_splice(k, d->txt, e->start, e->inserted_len, e->removed, e->removed_len);
        else text_splice(k, d->txt, e->start, e->removed_len, e->inserted, e->inserted_len);
    }
    if (req->blob_out && blob_len) memcpy(req->blob_out, blob, blob_len);
    from = d->version;
    d->version++;
    d->head = undo ? d->head - 1 : d->head + 1;
    d->coalesce_unit = 0;
    out->new_version = d->version;
    out->edit_count = (uint32_t)u->n;
    if (u->n == 1) {
        const edit_rec *e = &u->edits[0];
        out->start = e->start;
        out->removed = undo ? e->inserted_len : e->removed_len;
        out->inserted = undo ? e->removed_len : e->inserted_len;
    } else {
        out->start = out->removed = out->inserted = 0;
    }
    out->new_length = d->txt->len;
    out->blob_len = blob_len;
    emit_doc_event(k, d, undo ? CDIN_NOTIF_UNDO : CDIN_NOTIF_REDO, from, d->version, u->edits, u->n, undo, wl);
    return CDIN_OK;
}
