/* Generic handle pool: slot + generation + intrusive free list.
 *
 * Documents, transactions, snapshots and subscriptions are all "an index into
 * an array, and a released handle must never be valid again". This is the one
 * implementation; the four users differ only in the item. A handle packs a
 * slot index and a generation; release bumps the generation, so a stale handle
 * fails pool_get even though the slot is immediately reused. A slot whose
 * generation is exhausted (HANDLE_GEN_MAX) is retired and never handed out
 * again, which keeps the generation check honest. Not thread-safe: the kernel
 * lock covers every use (docs/architecture.md, “Locking and callback boundaries”). */
#include "internal.h"

cdin_status pool_insert(cdin_kernel *k, pool *pl, void *item, uint64_t *handle)
{
    size_t idx;
    pool_slot *s;
    if (pl->free_head) {
        idx = pl->free_head - 1u;
        pl->free_head = pl->slots[idx].next_free;
    } else {
        if (pl->used == pl->cap) {
            size_t ncap = pl->cap ? pl->cap * 2 : 8, ncap_bytes;
            pool_slot *ns;
            if (ncap > HANDLE_IDX_MASK) ncap = (size_t)HANDLE_IDX_MASK;
            if (ncap <= pl->cap) return CDIN_LIMIT_EXCEEDED;
            if (!k_mul(ncap, sizeof *ns, &ncap_bytes)) return CDIN_LIMIT_EXCEEDED;
            ns = pl->slots ? k_realloc(k, pl->slots, pl->cap * sizeof *ns, ncap_bytes)
                           : k_alloc(k, ncap_bytes);
            if (!ns) return CDIN_NO_MEMORY;
            pl->slots = ns;
            pl->cap = ncap;
        }
        idx = pl->used++;
        pl->slots[idx].gen = 1;
        pl->slots[idx].retired = 0;
        pl->slots[idx].next_free = 0;
    }
    s = &pl->slots[idx];
    s->item = item;
    pl->live++;
    *handle = (s->gen << HANDLE_IDX_BITS) | (uint64_t)idx;
    return CDIN_OK;
}

void *pool_get(const pool *pl, uint64_t handle)
{
    uint64_t idx = handle & HANDLE_IDX_MASK, gen = handle >> HANDLE_IDX_BITS;
    if (handle == 0 || idx >= pl->used) return NULL;
    if (pl->slots[idx].gen != gen) return NULL;
    return pl->slots[idx].item;
}

void *pool_remove(pool *pl, uint64_t handle)
{
    uint64_t idx = handle & HANDLE_IDX_MASK;
    void *item = pool_get(pl, handle);
    pool_slot *s;
    if (!item) return NULL;
    s = &pl->slots[idx];
    s->item = NULL;
    s->gen++;
    if (s->gen > HANDLE_GEN_MAX) {
        s->retired = 1;              /* generation exhausted: never reuse this slot */
    } else {
        s->next_free = pl->free_head;
        pl->free_head = (uint32_t)idx + 1u;
    }
    pl->live--;
    return item;
}
