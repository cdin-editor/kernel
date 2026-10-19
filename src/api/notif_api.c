/* Public wrappers for the notification group (NOTIF-*). Policy in notif/notif.c;
 * this file owns the lock/wakeup dance. */
#include "internal.h"

cdin_status cdin_notif_subscribe(cdin_kernel *k, cdin_doc d, const cdin_subscribe_request *req,
                                 cdin_version *out_version, cdin_notif *out)
{
    cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = notif_subscribe_locked(k, d, req, out_version, out);
    k_unlock(k);
    return st;
}

cdin_status cdin_notif_unsubscribe(cdin_kernel *k, cdin_notif n)
{
    cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = notif_unsubscribe_locked(k, n);
    k_unlock(k);
    return st;
}

cdin_status cdin_notif_drain(cdin_kernel *k, cdin_notif n, cdin_notif_event *events, size_t max,
                             size_t *out_count)
{
    cdin_status st;
    if (!k || !out_count) return CDIN_INVALID_ARGUMENT;
    if (max > 0 && !events) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    st = notif_drain_locked(k, n, events, max, out_count);
    k_unlock(k);
    return st;
}
