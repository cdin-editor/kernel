/* Public wrappers for the transaction group. Policy in txn/txn.c, publishing in
 * doc/edit.c (commit_finish); here: only lock, resolve and wake. */
#include "internal.h"

cdin_status cdin_txn_begin(cdin_kernel *k, cdin_doc d, const cdin_txn_begin_request *req, cdin_txn *out)
{
    doc *dd;
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_lookup(k, d, &dd);
    if (st == CDIN_OK) st = txn_begin_locked(k, dd, req, out);
    k_unlock(k);
    return st;
}

cdin_status cdin_txn_replace(cdin_kernel *k, cdin_txn t, const cdin_replace_request *req,
                             cdin_edit_result *out)
{
    txn_state *tx;
    doc *d;
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = txn_resolve(k, t, &tx, &d);
    if (st == CDIN_OK) st = txn_replace_locked(k, tx, d, req, out);
    k_unlock(k);
    return st;
}

cdin_status cdin_txn_commit(cdin_kernel *k, cdin_txn t, const cdin_txn_commit_request *req,
                            cdin_edit_result *out)
{
    txn_state *tx;
    doc *d;
    wake_list wl;
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    wl.n = 0;
    k_lock(k);
    st = txn_resolve(k, t, &tx, &d);
    if (st == CDIN_OK) st = txn_commit_locked(k, tx, d, req, out, &wl);
    k_unlock(k);
    wake_run(&wl);
    return st;
}

cdin_status cdin_txn_abort(cdin_kernel *k, cdin_txn t)
{
    txn_state *tx;
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = txn_resolve(k, t, &tx, &d);
    if (st == CDIN_OK) st = txn_abort_locked(k, tx, d);
    k_unlock(k);
    return st;
}

cdin_status cdin_txn_len(cdin_kernel *k, cdin_txn t, uint64_t *out)
{
    txn_state *tx;
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = txn_resolve(k, t, &tx, &d);
    if (st == CDIN_OK) st = txn_len_locked(k, tx, d, out);
    k_unlock(k);
    return st;
}

cdin_status cdin_txn_read(cdin_kernel *k, cdin_txn t, uint64_t start, uint64_t end,
                          uint8_t *buf, size_t bufsz, size_t *out_len)
{
    txn_state *tx;
    doc *d;
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = txn_resolve(k, t, &tx, &d);
    if (st == CDIN_OK) st = txn_read_locked(k, tx, d, start, end, buf, bufsz, out_len);
    k_unlock(k);
    return st;
}
