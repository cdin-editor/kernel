/* Change notifications (NOTIF-*): bounded per-subscriber ring buffers.
 *
 * Overflow never blocks a mutation and never loses information silently: when a ring is full
 * its content is replaced by one RESYNC event carrying the version to re-read at. Events that
 * arrive while a RESYNC is the newest queued event only advance its version.
 *
 * The public wrappers live in api/notif_api.c; everything here runs with the kernel
 * lock held and never calls user code (wakeups go through the wake list instead). */
#include "internal.h"

static void push(subscriber *s, const cdin_notif_event *ev, wake_list *wl)
{
    int closing = ev->kind == CDIN_NOTIF_CLOSED;
    if (s->doc_closed) return;
    if (!closing && s->count > 0) {
        cdin_notif_event *tail = &s->ring[(s->head + s->count - 1) % s->cap];
        if (tail->kind == CDIN_NOTIF_RESYNC) {              /* already invalidated: just move the version */
            if (ev->current_version > tail->current_version) tail->current_version = ev->current_version;
            wake_add(wl, s->wakeup, s->wakeup_ctx);
            return;
        }
    }
    if (s->count == s->cap) {                                 /* overflow: collapse to one RESYNC */
        cdin_notif_event rs;
        memset(&rs, 0, sizeof rs);
        rs.struct_size = (uint32_t)sizeof rs;
        rs.kind = CDIN_NOTIF_RESYNC;
        rs.doc = ev->doc;
        rs.from_version = ev->from_version;
        rs.to_version = ev->to_version;
        rs.current_version = ev->current_version;
        s->head = 0;
        s->count = 1;
        s->ring[0] = rs;
        if (!closing) { wake_add(wl, s->wakeup, s->wakeup_ctx); return; }
    }
    s->ring[(s->head + s->count) % s->cap] = *ev;            /* cap >= 4, so this always fits */
    s->count++;
    wake_add(wl, s->wakeup, s->wakeup_ctx);
}

static void each_sub(cdin_kernel *k, doc *d, const cdin_notif_event *ev, wake_list *wl)
{
    if (d->nsubs == 0) return;
    for (size_t i = 0; i < k->subs.used; i++) {
        subscriber *s = k->subs.slots[i].item;
        if (s && s->doc == d->handle) push(s, ev, wl);
    }
}

void emit_doc_event(cdin_kernel *k, doc *d, cdin_notif_kind kind, cdin_version from, cdin_version to,
                    const edit_rec *edits, size_t n, int inverse, wake_list *wl)
{
    cdin_notif_event ev;
    size_t stored = n < CDIN_NOTIF_MAX_EDITS ? n : CDIN_NOTIF_MAX_EDITS;
    if (d->nsubs == 0) return;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = (uint32_t)sizeof ev;
    ev.kind = kind;
    ev.doc = d->handle;
    ev.from_version = from;
    ev.to_version = to;
    ev.current_version = to;
    ev.edit_count = (uint32_t)n;
    ev.edits_stored = (uint32_t)stored;
    ev.edits_truncated = n > stored;
    for (size_t i = 0; i < stored; i++) {
        /* Undo applies the inverse of the unit's edits in reverse order. */
        const edit_rec *e = inverse ? &edits[n - 1 - i] : &edits[i];
        ev.edits[i].start = e->start;
        ev.edits[i].removed = inverse ? e->inserted_len : e->removed_len;
        ev.edits[i].inserted = inverse ? e->removed_len : e->inserted_len;
    }
    each_sub(k, d, &ev, wl);
}

void emit_simple(cdin_kernel *k, doc *d, cdin_notif_kind kind, uint64_t evicted, int is_clean, wake_list *wl)
{
    cdin_notif_event ev;
    if (d->nsubs == 0) return;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = (uint32_t)sizeof ev;
    ev.kind = kind;
    ev.doc = d->handle;
    ev.from_version = ev.to_version = ev.current_version = d->version;
    ev.evicted = evicted;
    ev.is_clean = is_clean;
    each_sub(k, d, &ev, wl);
}

void subs_detach_doc(cdin_kernel *k, doc *d)
{
    for (size_t i = 0; i < k->subs.used; i++) {
        subscriber *s = k->subs.slots[i].item;
        if (s && s->doc == d->handle) s->doc_closed = 1;
    }
    d->nsubs = 0;
}

cdin_status notif_subscribe_locked(cdin_kernel *k, cdin_doc dh, const cdin_subscribe_request *req,
                                   cdin_version *out_version, cdin_notif *out)
{
    doc *d;
    subscriber *s;
    cdin_status st;
    st = doc_lookup_mut(k, dh, &d);
    if (st != CDIN_OK) return st;
    st = check_struct(req, sizeof *req, sizeof *req);
    if (st != CDIN_OK) return st;
    if (req->flags != 0) return CDIN_UNSUPPORTED;
    if (d->nsubs >= k->max_subscribers) return CDIN_LIMIT_EXCEEDED;
    s = k_alloc(k, sizeof *s);
    if (!s) return CDIN_NO_MEMORY;
    memset(s, 0, sizeof *s);
    s->ring = k_alloc(k, (size_t)k->notif_capacity * sizeof *s->ring);
    if (!s->ring) { k_free(k, s, sizeof *s); return CDIN_NO_MEMORY; }
    s->cap = (size_t)k->notif_capacity;
    s->doc = dh;
    s->wakeup = req->wakeup;
    s->wakeup_ctx = req->wakeup_ctx;
    st = pool_insert(k, &k->subs, s, out);
    if (st != CDIN_OK) { k_free(k, s->ring, s->cap * sizeof *s->ring); k_free(k, s, sizeof *s); return st; }
    d->nsubs++;
    if (out_version) *out_version = d->version;
    return CDIN_OK;
}

/* NOTIF-010: pending events are discarded and the subscriber is detached, but
 * the handle stays valid (drain returns an empty queue) until the kernel is
 * destroyed; doc_closed doubles as the "detached" flag (no member needs to see
 * two states: neither CLOSED events nor further edits reach it). The ring is
 * freed here — the queue can never fill again — so churn costs one subscriber
 * shell, not one ring, per unsubscribe. */
cdin_status notif_unsubscribe_locked(cdin_kernel *k, cdin_notif nh)
{
    subscriber *s;
    s = pool_get(&k->subs, nh);
    if (!s) return CDIN_INVALID_HANDLE;
    if (!s->doc_closed) {
        doc *d = pool_get(&k->docs, s->doc);
        if (d && d->nsubs) d->nsubs--;
    }
    s->doc_closed = 1;
    s->head = 0;
    s->count = 0;
    k_free(k, s->ring, s->cap * sizeof *s->ring);   /* shortcut: a re-subscribed doc gets a NEW handle anyway */
    s->ring = NULL;
    s->cap = 0;
    return CDIN_OK;
}

cdin_status notif_drain_locked(cdin_kernel *k, cdin_notif nh, cdin_notif_event *events,
                               size_t max, size_t *out_count)
{
    subscriber *s;
    size_t n = 0;
    s = pool_get(&k->subs, nh);
    if (!s) return CDIN_INVALID_HANDLE;
    while (n < max && s->count > 0) {
        events[n++] = s->ring[s->head];
        s->head = (s->head + 1) % s->cap;
        s->count--;
    }
    if (s->count == 0) s->head = 0;
    *out_count = n;
    return CDIN_OK;
}
