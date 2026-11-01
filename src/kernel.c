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
#include <m-mdy-m/log.h>
#include <stdio.h>

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
            CDIN_FEAT_CASEFOLD_SIMPLE | CDIN_FEAT_LOCKING;
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

#define CDIN_KERNEL_LOG_PREFIX "[KERNEL] "

static int valid_cdin_log_level(cdin_log_level level)
{
    return level >= CDIN_LOG_ERROR && level <= CDIN_LOG_FATAL;
}

static int32_t mmlog_level_from_cdin(cdin_log_level level)
{
    switch (level) {
    case CDIN_LOG_TRACE: return MMLLOG_LEVEL_TRACE;
    case CDIN_LOG_DEBUG: return MMLLOG_LEVEL_DEBUG;
    case CDIN_LOG_INFO:  return MMLLOG_LEVEL_INFO;
    case CDIN_LOG_WARN:  return MMLLOG_LEVEL_WARN;
    case CDIN_LOG_ERROR: return MMLLOG_LEVEL_ERROR;
    case CDIN_LOG_FATAL: return MMLLOG_LEVEL_FATAL;
    default: return MMLLOG_LEVEL_ERROR;
    }
}

static cdin_log_level cdin_level_from_mmlog(int32_t level)
{
    switch (level) {
    case MMLLOG_LEVEL_TRACE: return CDIN_LOG_TRACE;
    case MMLLOG_LEVEL_DEBUG: return CDIN_LOG_DEBUG;
    case MMLLOG_LEVEL_INFO:  return CDIN_LOG_INFO;
    case MMLLOG_LEVEL_WARN:  return CDIN_LOG_WARN;
    case MMLLOG_LEVEL_ERROR: return CDIN_LOG_ERROR;
    case MMLLOG_LEVEL_FATAL: return CDIN_LOG_FATAL;
    default: return CDIN_LOG_ERROR;
    }
}

static uint32_t mmlog_format_flags_from_cdin(uint32_t flags)
{
    uint32_t result = 0;
    if (flags & CDIN_LOG_F_TIMESTAMP) result |= MMLLOG_F_TIMESTAMP;
    if (flags & CDIN_LOG_F_LAYER) result |= MMLLOG_F_LAYER;
    if (flags & CDIN_LOG_F_COMPONENT) result |= MMLLOG_F_COMPONENT;
    if (flags & CDIN_LOG_F_SOURCE) result |= MMLLOG_F_SOURCE;
    if (flags & CDIN_LOG_F_FUNCTION) result |= MMLLOG_F_FUNCTION;
    return result;
}

/* m-mdy-m/log.c calls sinks after releasing its own lock. The kernel callback
 * contract also requires that user code is never called under the kernel lock.
 * The prefix is added here (rather than stored as metadata) so every adapter,
 * including stdout/stderr and host callbacks, sees the same unambiguous tag. */
static void cdin_log_bridge(void *user_data, int32_t level,
                            const char *line, size_t line_length)
{
    cdin_kernel *k = (cdin_kernel *)user_data;
    const size_t prefix_length = sizeof CDIN_KERNEL_LOG_PREFIX - 1u;
    size_t total_length;
    char *buffer;
    FILE *stream = NULL;

    if (k == NULL || line == NULL) return;
    if (line_length > SIZE_MAX - prefix_length - 1u) return;
    total_length = prefix_length + line_length;
    /* The injected allocator may be shared with ordinary kernel calls. Hold
     * the kernel lock only around allocation/free, never around user sinks. */
    k_lock(k);
    buffer = (char *)k->al.alloc(k->al.ctx, total_length + 1u);
    k_unlock(k);
    if (buffer == NULL) return; /* diagnostics must never break a kernel operation */

    memcpy(buffer, CDIN_KERNEL_LOG_PREFIX, prefix_length);
    if (line_length != 0u) memcpy(buffer + prefix_length, line, line_length);
    buffer[total_length] = '\0';

    if (k->logs.log != NULL)
        k->logs.log(k->logs.ctx, (int)cdin_level_from_mmlog(level), buffer, total_length);

    if (k->log_options.output == CDIN_LOG_OUTPUT_STDERR ||
        k->log_options.output == CDIN_LOG_OUTPUT_BOTH) {
        stream = stderr;
        (void)fwrite(buffer, 1u, total_length, stream);
        (void)fputc('\n', stream);
        (void)fflush(stream);
    }
    if (k->log_options.output == CDIN_LOG_OUTPUT_STDOUT ||
        k->log_options.output == CDIN_LOG_OUTPUT_BOTH) {
        stream = stdout;
        (void)fwrite(buffer, 1u, total_length, stream);
        (void)fputc('\n', stream);
        (void)fflush(stream);
    }

    k_lock(k);
    k->al.free(k->al.ctx, buffer, total_length + 1u);
    k_unlock(k);
}

static cdin_status create_logger(cdin_kernel *k, const cdin_log_options *options,
                                 mmlog_logger_t **out_logger)
{
    mmlog_logger_config_t config;
    mmlog_logger_t *logger = NULL;
    mmlog_status_t status;

    if (k == NULL || options == NULL || out_logger == NULL) return CDIN_INVALID_ARGUMENT;
    if (options->output == CDIN_LOG_OUTPUT_CALLBACK && k->logs.log == NULL)
        return CDIN_INVALID_ARGUMENT;

    status = mmlog_logger_config_init(&config);
    if (status != MMLLOG_OK) return status == MMLLOG_ENOMEM ? CDIN_NO_MEMORY : CDIN_INTERNAL;
    config.min_level = (uint32_t)mmlog_level_from_cdin((cdin_log_level)options->min_level);
    config.output = MMLLOG_OUTPUT_NONE; /* the bridge owns prefixed console/callback output */
    config.color_mode = MMLLOG_COLOR_NEVER; /* stable plain text for IDE/runtime consumers */
    config.format_flags = mmlog_format_flags_from_cdin(options->format_flags);
    config.max_message_bytes = options->max_message_bytes;

    status = mmlog_logger_create(&config, &logger);
    if (status != MMLLOG_OK) return status == MMLLOG_ENOMEM ? CDIN_NO_MEMORY : CDIN_INTERNAL;
    status = mmlog_logger_add_callback(logger, cdin_log_bridge, k, MMLLOG_LEVEL_TRACE);
    if (status != MMLLOG_OK) {
        mmlog_logger_destroy(logger);
        return status == MMLLOG_ENOMEM ? CDIN_NO_MEMORY : CDIN_INTERNAL;
    }
    *out_logger = logger;
    return CDIN_OK;
}

cdin_status cdin_log_options_init(cdin_log_options *options)
{
    if (options == NULL) return CDIN_INVALID_ARGUMENT;
    memset(options, 0, sizeof *options);
    options->struct_size = (uint32_t)sizeof *options;
    options->min_level = CDIN_LOG_INFO;
    options->output = CDIN_LOG_OUTPUT_CALLBACK;
    options->format_flags = CDIN_LOG_F_DEFAULT;
    options->max_message_bytes = CDIN_LOG_DEFAULT_MAX_MESSAGE_BYTES;
    return CDIN_OK;
}

cdin_status cdin_kernel_set_log_options(cdin_kernel *k, const cdin_log_options *options)
{
    cdin_status status;
    cdin_log_options copy;
    mmlog_logger_t *new_logger = NULL;
    mmlog_logger_t *old_logger;
    const uint32_t allowed_flags = CDIN_LOG_F_TIMESTAMP | CDIN_LOG_F_LAYER |
                                   CDIN_LOG_F_COMPONENT | CDIN_LOG_F_SOURCE |
                                   CDIN_LOG_F_FUNCTION;

    if (k == NULL) return CDIN_INVALID_ARGUMENT;
    status = check_struct(options, sizeof *options, sizeof *options);
    if (status != CDIN_OK) return status;
    memcpy(&copy, options, sizeof copy);
    if (!valid_cdin_log_level((cdin_log_level)copy.min_level) ||
        copy.output > CDIN_LOG_OUTPUT_BOTH ||
        (copy.format_flags & ~allowed_flags) != 0u ||
        copy.max_message_bytes == 0u || copy.max_message_bytes > CDIN_LOG_MAX_MESSAGE_BYTES)
        return CDIN_INVALID_ARGUMENT;

    status = create_logger(k, &copy, &new_logger);
    if (status != CDIN_OK) return status;

    /* Configure before sharing the kernel with worker threads (documented API contract). */
    old_logger = k->logger;
    k->logger = new_logger;
    k->log_options = copy;
    if (old_logger != NULL) mmlog_logger_destroy(old_logger);
    K_LOG_INFO(k, "logger options configured");
    return CDIN_OK;
}

cdin_status cdin_kernel_set_log_level(cdin_kernel *k, cdin_log_level level)
{
    mmlog_status_t status;
    cdin_status result;
    if (k == NULL || !valid_cdin_log_level(level)) return CDIN_INVALID_ARGUMENT;
    k_lock(k);
    if (k->logger == NULL) {
        k_unlock(k);
        return CDIN_UNSUPPORTED;
    }
    status = mmlog_logger_set_min_level(k->logger, mmlog_level_from_cdin(level));
    if (status == MMLLOG_OK) {
        k->log_options.min_level = (uint32_t)level;
        result = CDIN_OK;
    } else {
        result = status == MMLLOG_EINVAL ? CDIN_INVALID_ARGUMENT : CDIN_INTERNAL;
    }
    k_unlock(k);
    return result;
}

static cdin_status emit_kernel_log(cdin_kernel *k, cdin_log_level level,
                                   const char *component, const char *message,
                                   const char *file, uint32_t line, const char *function)
{
    mmlog_event_t event;
    mmlog_status_t status;
    size_t message_length;

    if (k == NULL || message == NULL || !valid_cdin_log_level(level))
        return CDIN_INVALID_ARGUMENT;
    if (k->logger == NULL) return CDIN_UNSUPPORTED;
    message_length = strlen(message);
    status = mmlog_event_init(&event);
    if (status != MMLLOG_OK) return status == MMLLOG_ENOMEM ? CDIN_NO_MEMORY : CDIN_INTERNAL;
    event.level = mmlog_level_from_cdin(level);
    event.layer = "kernel";
    event.component = (component != NULL && component[0] != '\0') ? component : "cdin-kernel";
    event.file = file;
    event.line = line;
    event.function = function;
    event.message = message;
    event.message_length = message_length;
    status = mmlog_logger_write(k->logger, &event);
    if (status == MMLLOG_OK) return CDIN_OK;
    if (status == MMLLOG_ENOMEM) return CDIN_NO_MEMORY;
    if (status == MMLLOG_EINVAL || status == MMLLOG_ELIMIT) return CDIN_INVALID_ARGUMENT;
    return CDIN_INTERNAL;
}

cdin_status cdin_kernel_log(cdin_kernel *k, cdin_log_level level,
                            const char *component, const char *message)
{
    return emit_kernel_log(k, level, component, message, NULL, 0u, NULL);
}

void k_log_at(cdin_kernel *k, cdin_log_level level, const char *msg,
              const char *file, uint32_t line, const char *function)
{
    if (k == NULL || k->logger == NULL || msg == NULL) return;
    (void)emit_kernel_log(k, level, "cdin-kernel", msg, file, line, function);
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
    /* These vtables are embedded by value in the v1 config. Unlike the outer
     * config, they cannot carry an independently-sized extension tail. */
    if (cfg->allocator.struct_size != sizeof(cdin_allocator) || !cfg->allocator.alloc ||
        !cfg->allocator.realloc || !cfg->allocator.free)
        return CDIN_INVALID_ARGUMENT;
    if ((cfg->lock.lock == NULL) != (cfg->lock.unlock == NULL)) return CDIN_INVALID_ARGUMENT;
    if (cfg->lock.struct_size != 0 && cfg->lock.struct_size != sizeof(cdin_lock_vtable))
        return CDIN_INVALID_ARGUMENT;
    if (cfg->lock.struct_size == 0 && (cfg->lock.ctx || cfg->lock.lock || cfg->lock.unlock))
        return CDIN_INVALID_ARGUMENT;
    if (cfg->log.struct_size != 0 && cfg->log.struct_size != sizeof(cdin_log_sink))
        return CDIN_INVALID_ARGUMENT;
    if (cfg->log.struct_size == 0 && (cfg->log.ctx || cfg->log.log))
        return CDIN_INVALID_ARGUMENT;
    k = cfg->allocator.alloc(cfg->allocator.ctx, sizeof *k);
    if (!k) return CDIN_NO_MEMORY;
    memset(k, 0, sizeof *k);
    k->al = cfg->allocator;
    k->lock = cfg->lock;
    k->logs = cfg->log;
    (void)cdin_log_options_init(&k->log_options);
    if (k->logs.log != NULL) {
        st = create_logger(k, &k->log_options, &k->logger);
        if (st != CDIN_OK) {
            cdin_allocator allocator = k->al;
            allocator.free(allocator.ctx, k, sizeof *k);
            return st;
        }
    }
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
    K_LOG_INFO(k, "kernel initialized");
    return CDIN_OK;
}

cdin_status cdin_kernel_destroy(cdin_kernel *k)
{
    if (!k) return CDIN_INVALID_ARGUMENT;
    if (k->snaps.live > 0) {
        K_LOG_WARN(k, "kernel destroyed with live snapshots; releasing them");
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
    if (k->logger != NULL) mmlog_logger_destroy(k->logger);
    {
        cdin_allocator al = k->al;
        al.free(al.ctx, k, sizeof *k);
    }
    return CDIN_OK;
}

cdin_status cdin_kernel_last_sequence(cdin_kernel *k, uint64_t *out)
{
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    K_LOG_TRACE(k, "sequence queried");
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
    K_LOG_INFO(k, "kernel shutdown started");
    K_LOG_DEBUG(k, mode == CDIN_SHUTDOWN_ABORT ? "shutdown mode=abort" : "shutdown mode=graceful");
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
    K_LOG_TRACE(k, "kernel shutdown completed");
    return CDIN_OK;
}
