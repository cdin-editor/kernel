/* Kernel lifecycle: create, destroy, shutdown, sequence number, locking and
 * the version/status queries. The kernel group of the public header lives
 * here rather than in api/ because it has no module to wrap: it IS the
 * module. Everything else that is public goes through api/ wrappers.
 *
 * Shutdown marks the kernel STOPPED (later mutating calls fail with CDIN_SHUTDOWN,
 * CONC-020), aborts every active transaction (CONC-021) and returns. Documents are
 * NOT closed: reads and cdin_doc_close keep working, so a caller can finish its
 * teardown in any order; cdin_kernel_destroy frees any leftovers. */
#include "internal.h"

/* ---- version / feature queries --------------------------------------------- */
const char *cdin_status_name(cdin_status s)
{
    switch (s) {
    case CDIN_OK: return "CDIN_OK";
    case CDIN_INVALID_ARGUMENT: return "CDIN_INVALID_ARGUMENT";
    case CDIN_INVALID_HANDLE: return "CDIN_INVALID_HANDLE";
    case CDIN_STALE_VERSION: return "CDIN_STALE_VERSION";
    case CDIN_OUT_OF_RANGE: return "CDIN_OUT_OF_RANGE";
    case CDIN_INVALID_POSITION: return "CDIN_INVALID_POSITION";
    case CDIN_INVALID_STATE: return "CDIN_INVALID_STATE";
    case CDIN_BUSY: return "CDIN_BUSY";
    case CDIN_LIMIT_EXCEEDED: return "CDIN_LIMIT_EXCEEDED";
    case CDIN_NO_MEMORY: return "CDIN_NO_MEMORY";
    case CDIN_PATTERN_INVALID: return "CDIN_PATTERN_INVALID";
    case CDIN_CANCELLED: return "CDIN_CANCELLED";
    case CDIN_SHUTDOWN: return "CDIN_SHUTDOWN";
    case CDIN_UNSUPPORTED: return "CDIN_UNSUPPORTED";
    case CDIN_BUFFER_TOO_SMALL: return "CDIN_BUFFER_TOO_SMALL";
    case CDIN_VERSION_MISMATCH: return "CDIN_VERSION_MISMATCH";
    case CDIN_INTERNAL: return "CDIN_INTERNAL";
    }
    return "CDIN_UNKNOWN";
}

cdin_status cdin_abi_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (!major || !minor || !patch) return CDIN_INVALID_ARGUMENT;
    *major = CDIN_KERNEL_ABI_MAJOR; *minor = CDIN_KERNEL_ABI_MINOR; *patch = CDIN_KERNEL_ABI_PATCH;
    return CDIN_OK;
}

cdin_status cdin_features(uint64_t *mask)
{
    if (!mask) return CDIN_INVALID_ARGUMENT;
    *mask = CDIN_FEAT_BATCH | CDIN_FEAT_TRANSACTIONS | CDIN_FEAT_SNAPSHOTS | CDIN_FEAT_NOTIFICATIONS |
            CDIN_FEAT_SEARCH_LITERAL | CDIN_FEAT_SEARCH_BYTES | CDIN_FEAT_GRAPHEME |
            CDIN_FEAT_CASEFOLD_SIMPLE;
    return CDIN_OK;
}

cdin_status cdin_unicode_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (!major || !minor || !patch) return CDIN_INVALID_ARGUMENT;
    *major = cdin_unicode_major; *minor = cdin_unicode_minor; *patch = cdin_unicode_patch;
    return CDIN_OK;
}

/* ---- locking and logging ------------------------------------------------- */
void k_lock(cdin_kernel *k) { if (k->lock.lock) k->lock.lock(k->lock.ctx); }
void k_unlock(cdin_kernel *k) { if (k->lock.unlock) k->lock.unlock(k->lock.ctx); }

void k_log(cdin_kernel *k, int level, const char *msg)
{
    if (k->logs.log) k->logs.log(k->logs.ctx, level, msg, strlen(msg));
}

/* ---- caller struct validation (ABI-003) ---------------------------------- */
cdin_status check_struct(const void *p, size_t known_size, size_t min_size)
{
    const unsigned char *b = p;
    uint32_t declared;
    if (!p) return CDIN_INVALID_ARGUMENT;
    memcpy(&declared, b, sizeof declared);
    if (declared < min_size) return CDIN_INVALID_ARGUMENT;
    for (size_t i = known_size; i < declared; i++)
        if (b[i] != 0) return CDIN_UNSUPPORTED;
    return CDIN_OK;
}

/* ---- kernel create / destroy ---------------------------------------------- */
static uint64_t or_default(uint64_t v, uint64_t def) { return v ? v : def; }

cdin_status cdin_kernel_create(const cdin_kernel_config *cfg, cdin_kernel **out)
{
    cdin_kernel *k;
    cdin_status st;
    uint64_t max_size_cap = (uint64_t)(SIZE_MAX >> 2);
    if (!out) return CDIN_INVALID_ARGUMENT;
    st = check_struct(cfg, sizeof(cdin_kernel_config), sizeof(cdin_kernel_config));
    if (st != CDIN_OK) return st;
    if (cfg->abi_major != CDIN_KERNEL_ABI_MAJOR) return CDIN_VERSION_MISMATCH;
    if (cfg->allocator.struct_size < sizeof(cdin_allocator) || !cfg->allocator.alloc ||
        !cfg->allocator.realloc || !cfg->allocator.free)
        return CDIN_INVALID_ARGUMENT;
    if ((cfg->lock.lock == NULL) != (cfg->lock.unlock == NULL)) return CDIN_INVALID_ARGUMENT;
    k = cfg->allocator.alloc(cfg->allocator.ctx, sizeof *k);
    if (!k) return CDIN_NO_MEMORY;
    memset(k, 0, sizeof *k);
    k->al = cfg->allocator;
    k->lock = cfg->lock;
    k->logs = cfg->log;
    k->max_documents = or_default(cfg->max_documents, CDIN_DEFAULT_MAX_DOCUMENTS);
    k->max_document_size = or_default(cfg->max_document_size, CDIN_DEFAULT_MAX_DOCUMENT_SIZE);
    if (k->max_document_size > max_size_cap) k->max_document_size = max_size_cap;
    k->max_edit_size = or_default(cfg->max_edit_size, k->max_document_size);
    if (k->max_edit_size > k->max_document_size) k->max_edit_size = k->max_document_size;
    k->max_undo_memory = or_default(cfg->max_undo_memory, CDIN_DEFAULT_MAX_UNDO_MEMORY);
    k->max_blob_size = or_default(cfg->max_blob_size, CDIN_DEFAULT_MAX_BLOB_SIZE);
    k->max_snapshots = or_default(cfg->max_snapshots_per_doc, CDIN_DEFAULT_MAX_SNAPSHOTS);
    k->max_subscribers = or_default(cfg->max_subscribers_per_doc, CDIN_DEFAULT_MAX_SUBSCRIBERS);
    if (k->max_subscribers > CDIN_MAX_SUBSCRIBERS_HARD) k->max_subscribers = CDIN_MAX_SUBSCRIBERS_HARD;
    k->notif_capacity = or_default(cfg->notif_queue_capacity, CDIN_DEFAULT_NOTIF_QUEUE);
    if (k->notif_capacity < 4u) k->notif_capacity = 4u;          /* RESYNC + CLOSED must always fit */
    if (k->notif_capacity > 65536u) k->notif_capacity = 65536u;
    k->default_steps = or_default(cfg->default_search_steps, CDIN_DEFAULT_SEARCH_STEPS);
    k->seq = 0;
    *out = k;
    return CDIN_OK;
}

cdin_status cdin_kernel_destroy(cdin_kernel *k)
{
    if (!k) return CDIN_INVALID_ARGUMENT;
    if (k->snaps.live > 0) {
        k_log(k, 1, "kernel destroyed with live snapshots; releasing them");
    }
    for (size_t i = 0; i < k->docs.used; i++) {
        doc *d = k->docs.slots[i].item;
        if (d) {
            doc_free_contents(k, d);
            k_free(k, d, sizeof *d);
        }
    }
    for (size_t i = 0; i < k->snaps.used; i++) {
        snapshot *s = k->snaps.slots[i].item;
        if (s) {
            text_release(k, s->txt);
            k_free(k, s, sizeof *s);
        }
    }
    for (size_t i = 0; i < k->subs.used; i++) {
        subscriber *s = k->subs.slots[i].item;
        if (s) {
            k_free(k, s->ring, s->cap * sizeof *s->ring);
            k_free(k, s, sizeof *s);
        }
    }
    k_free(k, k->docs.slots, k->docs.cap * sizeof(pool_slot));
    k_free(k, k->snaps.slots, k->snaps.cap * sizeof(pool_slot));
    k_free(k, k->subs.slots, k->subs.cap * sizeof(pool_slot));
    k_free(k, k->txns.slots, k->txns.cap * sizeof(pool_slot));
    {
        cdin_allocator al = k->al;
        al.free(al.ctx, k, sizeof *k);
    }
    return CDIN_OK;
}

cdin_status cdin_kernel_last_sequence(cdin_kernel *k, uint64_t *out)
{
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    *out = k->seq;
    k_unlock(k);
    return CDIN_OK;
}

/* CONC-020/021: stop accepting mutations, abort active transactions. The
 * documents themselves stay open (contexts may still be read and closed in
 * any order); destroy reclaims whatever survives. */
cdin_status cdin_kernel_shutdown(cdin_kernel *k, cdin_shutdown_mode mode)
{
    if (!k) return CDIN_INVALID_ARGUMENT;
    if (mode != CDIN_SHUTDOWN_GRACEFUL && mode != CDIN_SHUTDOWN_ABORT) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    k->stopped = 1;                    /* new mutations now fail with CDIN_SHUTDOWN */
    for (size_t i = 0; i < k->docs.used; i++) {
        doc *d = k->docs.slots[i].item;
        if (d && d->tx) {
            txn_state *tx = d->tx;
            pool_remove(&k->txns, tx->handle);
            d->tx = NULL;              /* first: no re-entrancy through the doc */
            txn_free(k, tx);           /* rollback == discard the tentative block */
        }
    }
    k_unlock(k);
    return CDIN_OK;
}
