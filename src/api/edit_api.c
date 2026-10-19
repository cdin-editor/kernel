/* Public wrappers for the edit group (replace, batch, undo, redo).
 * All policy lives in doc/edit.c; this file owns only the lock and the wakeups. */
#include "internal.h"

cdin_status cdin_doc_replace(cdin_kernel *k, cdin_doc d, const cdin_replace_request *req,
                             cdin_edit_result *out)
{
    wake_list wl;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = doc_replace_locked(k, d, req, out, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_doc_replace_batch(cdin_kernel *k, cdin_doc d, const cdin_batch_request *req,
                                   cdin_edit_result *out)
{
    wake_list wl;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = doc_batch_locked(k, d, req, out, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_doc_undo(cdin_kernel *k, cdin_doc d, const cdin_history_request *req,
                          cdin_edit_result *out)
{
    wake_list wl;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = history_locked(k, d, req, out, 1, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_doc_redo(cdin_kernel *k, cdin_doc d, const cdin_history_request *req,
                          cdin_edit_result *out)
{
    wake_list wl;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = history_locked(k, d, req, out, 0, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}
