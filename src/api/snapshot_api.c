/* Public wrappers for the snapshot group (CONC-005/006). Policy in doc/doc.c. */
#include "internal.h"

cdin_status cdin_snapshot_acquire(cdin_kernel *k, cdin_doc d, cdin_version *out_version, cdin_snapshot *out)
{
    doc *dd;
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = doc_lookup_mut(k, d, &dd);
    if (st == CDIN_OK) st = snapshot_acquire_locked(k, dd, out_version, out);
    k_unlock(k);
    return st;
}

cdin_status cdin_snapshot_release(cdin_kernel *k, cdin_snapshot sh)
{
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = snapshot_release_locked(k, sh);
    k_unlock(k);
    return st;
}
