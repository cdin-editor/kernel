/* Public wrappers for the document group of the header: lock, resolve, call
 * the module's *_locked function, release, run the wakeups. All policy lives
 * in doc/doc.c; this file owns only the concurrency dance (ABI-012, ABI-016). */
#include "internal.h"

cdin_status cdin_doc_create(cdin_kernel *k, const uint8_t *bytes, size_t len, cdin_doc *out)
{
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_create_locked(k, bytes, len, out);
    k_unlock(k);
    return st;
}

cdin_status cdin_doc_close(cdin_kernel *k, cdin_doc dh)
{
    wake_list wl;
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = doc_lookup(k, dh, &d);
    if (st == CDIN_OK) st = doc_close_locked(k, d, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_doc_reload(cdin_kernel *k, cdin_doc dh, const cdin_reload_request *req, cdin_edit_result *out)
{
    wake_list wl;
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = out_check(out);
    if (st == CDIN_OK) st = doc_lookup_mut(k, dh, &d);
    if (st == CDIN_OK) { details_clear(out); st = doc_reload_locked(k, d, req, out, &wl); }
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_doc_mark_clean(cdin_kernel *k, cdin_doc dh)
{
    wake_list wl;
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = doc_lookup_mut(k, dh, &d);
    if (st == CDIN_OK) st = doc_mark_clean_locked(k, d, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_doc_is_clean(cdin_kernel *k, cdin_doc dh, int *out)
{
    doc *d;
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_lookup(k, dh, &d);
    if (st == CDIN_OK) *out = doc_is_clean(d);
    k_unlock(k);
    return st;
}

cdin_status cdin_doc_set_metadata(cdin_kernel *k, cdin_doc dh, uint64_t metadata)
{
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_lookup_mut(k, dh, &d);
    if (st == CDIN_OK) d->metadata = metadata;
    k_unlock(k);
    return st;
}

cdin_status cdin_doc_get_metadata(cdin_kernel *k, cdin_doc dh, uint64_t *out)
{
    doc *d;
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_lookup(k, dh, &d);
    if (st == CDIN_OK) *out = d->metadata;
    k_unlock(k);
    return st;
}

cdin_status cdin_doc_history_info(cdin_kernel *k, cdin_doc dh, uint64_t *undo_units, uint64_t *redo_units,
                                  uint64_t *evicted_total, uint64_t *memory_bytes)
{
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_lookup(k, dh, &d);
    if (st == CDIN_OK) st = doc_history_info_locked(k, d, undo_units, redo_units, evicted_total, memory_bytes);
    k_unlock(k);
    return st;
}
