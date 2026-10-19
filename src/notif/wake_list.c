/* Wakeup lists (ABI-016).
 *
 * A wakeup callback is caller code, so it must never run under the kernel
 * lock. Mutations collect the subscribers that need signalling into a list
 * while holding the lock and run them after it is released. add() is
 * deduplicated: a subscriber whose ring is collapsed into a RESYNC and then
 * filled again must be signalled once, not once per event. */
#include "internal.h"

void wake_run(wake_list *wl)
{
    for (size_t i = 0; i < wl->n; i++) wl->fn[i](wl->ctx[i]);
    wl->n = 0;
}

void wake_add(wake_list *wl, void (*fn)(void *), void *ctx)
{
    if (!fn) return;
    for (size_t i = 0; i < wl->n; i++)
        if (wl->fn[i] == fn && wl->ctx[i] == ctx) return;
    if (wl->n < WAKE_MAX) { wl->fn[wl->n] = fn; wl->ctx[wl->n] = ctx; wl->n++; }
}
