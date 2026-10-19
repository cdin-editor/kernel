/* Undo unit bookkeeping (UNDO-*): the array of units, its cap, the eviction
 * policy and the saved-state marker.
 *
 * Independent of the kernel in the sense that nothing here emits events,
 * touches notifications or publishes versions: the doc/edit layer decides
 * WHEN a unit is committed, merged or dropped; this file owns the data
 * structure rules:
 *  - UNDO-003/030: eviction always drops the OLDEST unit and never the newest
 *    (an edit that cannot be undone is data loss, UNDO-031);
 *  - UNDO-040: the saved-state marker is a unit id; if that unit is evicted
 *    the document counts as modified (marker_lost) until it is marked again;
 *  - the bytes of a unit are accounted exactly so the memory cap is honest. */
#include "internal.h"

size_t edit_bytes(const edit_rec *e) { return e->removed_len + e->inserted_len; }

void edits_free(cdin_kernel *k, edit_rec *e, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        k_free(k, e[i].removed, e[i].removed_len);
        k_free(k, e[i].inserted, e[i].inserted_len);
    }
}

void unit_free(cdin_kernel *k, unit *u)
{
    edits_free(k, u->edits, u->n);
    k_free(k, u->edits, u->cap * sizeof(edit_rec));
    k_free(k, u->blob_before, u->blob_before_len);
    k_free(k, u->blob_after, u->blob_after_len);
    memset(u, 0, sizeof *u);
}

int doc_is_clean(const doc *d)
{
    uint64_t head_id = d->head ? d->units[d->head - 1].id : 0;
    return !d->marker_lost && head_id == d->marker_id;
}

/* Builds the inverse record of replacing [start,end) with bytes. Copies; NO_MEMORY on failure. */
cdin_status edit_rec_make(cdin_kernel *k, const text *t, uint64_t start, uint64_t end,
                          const uint8_t *bytes, size_t len, edit_rec *r)
{
    memset(r, 0, sizeof *r);
    r->start = start;
    if (end > start) {
        r->removed = k_dup(k, t->data + start, (size_t)(end - start));
        if (!r->removed) return CDIN_NO_MEMORY;
        r->removed_len = (size_t)(end - start);
    }
    if (len) {
        r->inserted = k_dup(k, bytes, len);
        if (!r->inserted) { k_free(k, r->removed, r->removed_len); memset(r, 0, sizeof *r); return CDIN_NO_MEMORY; }
        r->inserted_len = len;
    }
    return CDIN_OK;
}

/* Grows the unit array to `want` slots. */
cdin_status units_reserve(cdin_kernel *k, doc *d, size_t want)
{
    if (want > d->units_cap) {
        size_t ncap = d->units_cap ? d->units_cap * 2 : 8, ncap_bytes;
        unit *nu;
        if (ncap < want) ncap = want;
        if (k_mul(ncap, sizeof(unit), &ncap_bytes) == 0) return CDIN_LIMIT_EXCEEDED;
        nu = d->units ? k_realloc(k, d->units, d->units_cap * sizeof(unit), ncap_bytes)
                      : k_alloc(k, ncap_bytes);
        if (!nu) return CDIN_NO_MEMORY;
        d->units = nu;
        d->units_cap = ncap;
    }
    return CDIN_OK;
}

/* UNDO-003: discard the redo tail before recording a new edit. */
void units_drop_redo(cdin_kernel *k, doc *d)
{
    for (size_t i = d->head; i < d->nunits; i++) {
        if (d->units[i].id == d->marker_id) d->marker_lost = 1;
        d->undo_bytes -= d->units[i].bytes;
        unit_free(k, &d->units[i]);
    }
    d->nunits = d->head;
}

void evict_oldest(cdin_kernel *k, doc *d)
{
    unit *u = &d->units[0];
    /* UNDO-040: if the saved-state marker's unit is evicted the document is modified until it is
     * marked saved again. Marker 0 is the initial state, which is unreachable once the oldest unit is gone. */
    if (d->marker_id == u->id || d->marker_id == 0) d->marker_lost = 1;
    if (d->coalesce_unit == u->id) d->coalesce_unit = 0;
    d->undo_bytes -= u->bytes;
    unit_free(k, u);
    memmove(&d->units[0], &d->units[1], (d->nunits - 1) * sizeof(unit));
    d->nunits--;
    d->head--;
}
