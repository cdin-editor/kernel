/*
 * cdin-kernel: generic text engine, C ABI (draft 0.2.0).
 *
 * The ONLY public header. Rules (spec: ABI-001..ABI-022):
 *  - every function returns cdin_status; results go through out-parameters;
 *  - every struct passed INTO the kernel starts with `struct_size`;
 *  - memory returned to the caller is always copied into caller-provided
 *    buffers (two-call pattern: pass buf == NULL to learn the size);
 *  - on failure out-parameters are left untouched, except the `details`
 *    block of cdin_edit_result, which reports why the call failed;
 *  - zero in a configuration limit means "documented default".
 *
 * Thread safety (ABI-012): with a lock supplied in cdin_kernel_config every
 * function is ANY_THREAD. Without a lock the kernel is OWNER_THREAD: all calls
 * must come from one thread at a time. cdin_kernel_destroy is always
 * OWNER_THREAD (no other call may run concurrently or afterwards).
 * Callbacks (wakeup, log) are never invoked while the kernel lock is held.
 */
#ifndef CDIN_KERNEL_H
#define CDIN_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(CDIN_KERNEL_BUILD_DLL)
#    define CDIN_API __declspec(dllexport)
#  else
#    define CDIN_API
#  endif
#elif defined(__GNUC__)
#  define CDIN_API __attribute__((visibility("default")))
#else
#  define CDIN_API
#endif

#define CDIN_KERNEL_ABI_MAJOR 0u
#define CDIN_KERNEL_ABI_MINOR 2u
#define CDIN_KERNEL_ABI_PATCH 0u

/* ---- status (ERR-001..ERR-012). Numeric values are frozen once released. -- */
typedef enum cdin_status {
    CDIN_OK = 0,
    CDIN_INVALID_ARGUMENT,
    CDIN_INVALID_HANDLE,
    CDIN_STALE_VERSION,
    CDIN_OUT_OF_RANGE,
    CDIN_INVALID_POSITION,
    CDIN_INVALID_STATE,
    CDIN_BUSY,
    CDIN_LIMIT_EXCEEDED,
    CDIN_NO_MEMORY,
    CDIN_PATTERN_INVALID,
    CDIN_CANCELLED,
    CDIN_SHUTDOWN,
    CDIN_UNSUPPORTED,
    CDIN_BUFFER_TOO_SMALL,
    CDIN_VERSION_MISMATCH,
    CDIN_INTERNAL
} cdin_status;

/* Static, UTF-8, NUL-terminated name such as "CDIN_STALE_VERSION". */
CDIN_API const char *cdin_status_name(cdin_status s);

/* ---- handles (ABI-004/005, DOC-005) ------------------------------------- */
typedef struct cdin_kernel cdin_kernel;
typedef uint64_t cdin_doc;       /* 0 is never valid */
typedef uint64_t cdin_txn;       /* 0 is never valid */
typedef uint64_t cdin_snapshot;  /* 0 is never valid */
typedef uint64_t cdin_notif;     /* 0 is never valid */
typedef uint64_t cdin_version;   /* new documents start at 0 */

#define CDIN_INVALID_DOC      ((cdin_doc)0)
#define CDIN_INVALID_TXN      ((cdin_txn)0)
#define CDIN_INVALID_SNAPSHOT ((cdin_snapshot)0)
#define CDIN_INVALID_NOTIF    ((cdin_notif)0)
#define CDIN_ANY_VERSION      UINT64_MAX

/* A read-only source of text: exactly one member is non-zero. */
typedef struct cdin_source {
    cdin_doc doc;
    cdin_snapshot snapshot;
} cdin_source;

static inline cdin_source cdin_src_doc(cdin_doc d) { cdin_source s = { d, 0 }; return s; }
static inline cdin_source cdin_src_snap(cdin_snapshot n) { cdin_source s = { 0, n }; return s; }

/* ---- injected services (ABI-016/017, ERR-030) ---------------------------- */
typedef struct cdin_allocator {
    uint32_t struct_size;
    void *(*alloc)(void *ctx, size_t n);                           /* n >= 1 */
    void *(*realloc)(void *ctx, void *p, size_t old_n, size_t new_n); /* p != NULL, sizes >= 1 */
    void (*free)(void *ctx, void *p, size_t n);                    /* n is the exact current size */
    void *ctx;
} cdin_allocator;

/* Optional. NULL lock/unlock means "single-threaded use" (OWNER_THREAD). */
typedef struct cdin_lock_vtable {
    uint32_t struct_size;
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} cdin_lock_vtable;

typedef struct cdin_log_sink {
    uint32_t struct_size;
    void *ctx;
    /* level: 0 error, 1 warning, 2 info. msg is UTF-8, not NUL-terminated. */
    void (*log)(void *ctx, int level, const char *msg, size_t len);
} cdin_log_sink;

/* Identifiers of limits, reported in cdin_error_details.limit_id. */
enum {
    CDIN_LIMIT_NONE = 0,
    CDIN_LIMIT_DOCUMENTS = 1,
    CDIN_LIMIT_DOCUMENT_SIZE = 2,
    CDIN_LIMIT_EDIT_SIZE = 3,
    CDIN_LIMIT_UNDO_MEMORY = 4,
    CDIN_LIMIT_BLOB_SIZE = 5,
    CDIN_LIMIT_SNAPSHOTS = 6,
    CDIN_LIMIT_SUBSCRIBERS = 7,
    CDIN_LIMIT_SEARCH_STEPS = 8,
    CDIN_LIMIT_TXN_MEMORY = 9
};

/* Defaults used when a limit is 0 (docs/limits-and-status.md). */
#define CDIN_DEFAULT_MAX_DOCUMENTS        1024u
#define CDIN_DEFAULT_MAX_DOCUMENT_SIZE    ((uint64_t)1 << 30)
#define CDIN_DEFAULT_MAX_UNDO_MEMORY      ((uint64_t)64 << 20)
#define CDIN_DEFAULT_MAX_BLOB_SIZE        4096u
#define CDIN_DEFAULT_MAX_SNAPSHOTS        16u
#define CDIN_DEFAULT_MAX_SUBSCRIBERS      16u
#define CDIN_MAX_SUBSCRIBERS_HARD         64u
#define CDIN_DEFAULT_NOTIF_QUEUE          64u
#define CDIN_DEFAULT_SEARCH_STEPS         ((uint64_t)1 << 32)

typedef struct cdin_kernel_config {
    uint32_t struct_size;
    uint32_t abi_major;              /* caller's expected ABI major; mismatch -> VERSION_MISMATCH */
    cdin_allocator allocator;
    cdin_lock_vtable lock;           /* optional */
    cdin_log_sink log;               /* optional */
    uint64_t max_documents;          /* live documents */
    uint64_t max_document_size;      /* bytes */
    uint64_t max_edit_size;          /* bytes inserted by one edit; 0 = max_document_size */
    uint64_t max_undo_memory;        /* bytes, per document */
    uint64_t max_blob_size;          /* bytes, per blob */
    uint64_t max_snapshots_per_doc;
    uint64_t max_subscribers_per_doc;
    uint64_t notif_queue_capacity;   /* events, per subscriber */
    uint64_t default_search_steps;   /* used when a request has step_limit 0 */
} cdin_kernel_config;

/* ---- details of a failure (ERR-003, ERR-010) ------------------------------ */
typedef struct cdin_error_details {
    uint32_t struct_size;
    uint32_t limit_id;     /* LIMIT_EXCEEDED: which CDIN_LIMIT_* */
    uint64_t expected;     /* STALE_VERSION */
    uint64_t current;      /* STALE_VERSION */
    uint64_t requested;    /* OUT_OF_RANGE / INVALID_POSITION: offending value */
    uint64_t length;       /* OUT_OF_RANGE: document length */
    uint64_t required;     /* BUFFER_TOO_SMALL: bytes needed */
} cdin_error_details;

/* ---- feature and version queries (ABI-014) ------------------------------- */
#define CDIN_FEAT_BATCH          (1ull << 0)
#define CDIN_FEAT_TRANSACTIONS   (1ull << 1)
#define CDIN_FEAT_SNAPSHOTS      (1ull << 2)
#define CDIN_FEAT_NOTIFICATIONS  (1ull << 3)
#define CDIN_FEAT_SEARCH_LITERAL (1ull << 4)
#define CDIN_FEAT_SEARCH_BYTES   (1ull << 5)
#define CDIN_FEAT_SEARCH_REGEX   (1ull << 6)   /* NOT set in this release */
#define CDIN_FEAT_GRAPHEME       (1ull << 7)
#define CDIN_FEAT_CASEFOLD_SIMPLE (1ull << 8)
#define CDIN_FEAT_LOCKING        (1ull << 9)   /* reserved here; cdin_features() currently never sets it */

CDIN_API cdin_status cdin_abi_version(uint32_t *major, uint32_t *minor, uint32_t *patch);
/* Static feature set of this build (CDIN_FEAT_LOCKING is never set here). */
CDIN_API cdin_status cdin_features(uint64_t *mask);
/* Pinned Unicode version of this release (POS-032). */
CDIN_API cdin_status cdin_unicode_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/* ---- kernel lifecycle ----------------------------------------------------- */
CDIN_API cdin_status cdin_kernel_create(const cdin_kernel_config *cfg, cdin_kernel **out);
/* Frees everything, including live documents, snapshots and subscriptions.
 * Leaked snapshots are reported through the log sink (CONC-022). */
CDIN_API cdin_status cdin_kernel_destroy(cdin_kernel *k);

typedef enum cdin_shutdown_mode {
    CDIN_SHUTDOWN_GRACEFUL = 0,
    CDIN_SHUTDOWN_ABORT = 1
} cdin_shutdown_mode;

/* Stops accepting content mutations and aborts active transactions. Documents
 * remain readable and may be closed; snapshot reads/release and notification
 * drain/unsubscribe remain available. Destroy reclaims remaining state.
 * Calls run inline under the kernel lock, so no accepted request is pending:
 * both modes behave identically today (see docs/limits-and-status.md). */
CDIN_API cdin_status cdin_kernel_shutdown(cdin_kernel *k, cdin_shutdown_mode mode);

/* Sequence number of the last accepted mutation (CONC-004). */
CDIN_API cdin_status cdin_kernel_last_sequence(cdin_kernel *k, uint64_t *out);

/* ---- documents ------------------------------------------------------------ */
CDIN_API cdin_status cdin_doc_create(cdin_kernel *k, const uint8_t *bytes, size_t len, cdin_doc *out);
/* Ordered request: aborts an active transaction, emits `closed`. */
CDIN_API cdin_status cdin_doc_close(cdin_kernel *k, cdin_doc d);

typedef struct cdin_reload_request {
    uint32_t struct_size;
    uint32_t flags;               /* CDIN_RELOAD_KEEP_HISTORY: not supported yet -> UNSUPPORTED */
    uint64_t expected_version;
    const uint8_t *bytes;
    size_t len;
} cdin_reload_request;
#define CDIN_RELOAD_KEEP_HISTORY 1u

/* ---- edits ---------------------------------------------------------------- */
typedef struct cdin_replace_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved, must be 0 */
    uint64_t expected_version;    /* or CDIN_ANY_VERSION */
    uint64_t start, end;          /* byte offsets, half-open */
    const uint8_t *bytes;
    size_t len;
    uint64_t coalesce_key;        /* 0 = none (UNDO-010) */
    const uint8_t *blob_before;   /* opaque caller state restored on undo (UNDO-020) */
    size_t blob_before_len;
    const uint8_t *blob_after;    /* ... restored on redo */
    size_t blob_after_len;
} cdin_replace_request;

typedef struct cdin_edit_result {
    uint32_t struct_size;         /* set by the caller to sizeof */
    uint32_t edit_count;          /* number of primitive edits applied */
    cdin_version new_version;     /* equals the old version for a no-op */
    uint64_t accepted_seq;        /* CONC-004; 0 if the request was not accepted */
    /* Valid only when edit_count == 1 (otherwise 0); coordinates of the
     * previous version (EDIT-012). Multi-edit changes are described by the
     * change notification. */
    uint64_t start, removed, inserted;
    uint64_t new_length;
    uint64_t blob_len;            /* undo/redo: size of the blob belonging to the unit */
    cdin_error_details details;   /* written on failure */
} cdin_edit_result;

CDIN_API cdin_status cdin_doc_replace(cdin_kernel *k, cdin_doc d,
                                      const cdin_replace_request *req, cdin_edit_result *out);
CDIN_API cdin_status cdin_doc_reload(cdin_kernel *k, cdin_doc d,
                                     const cdin_reload_request *req, cdin_edit_result *out);

/* EDIT-005: several ranges against the same version, one undo unit. */
typedef struct cdin_batch_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved, must be 0 */
    uint64_t expected_version;
    size_t count;
    const uint64_t *starts;       /* count entries, ascending, non-overlapping */
    const uint64_t *ends;
    const uint8_t *const *bytes;  /* count pointers (may be NULL where lens[i] == 0) */
    const size_t *lens;
    uint64_t coalesce_key;        /* must be 0 for batches */
    const uint8_t *blob_before;
    size_t blob_before_len;
    const uint8_t *blob_after;
    size_t blob_after_len;
} cdin_batch_request;

CDIN_API cdin_status cdin_doc_replace_batch(cdin_kernel *k, cdin_doc d,
                                            const cdin_batch_request *req, cdin_edit_result *out);

/* ---- undo / redo ---------------------------------------------------------- */
typedef struct cdin_history_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    uint64_t expected_version;
    uint8_t *blob_out;            /* optional: receives the unit's blob (before on undo, after on redo) */
    size_t blob_capacity;
} cdin_history_request;

CDIN_API cdin_status cdin_doc_undo(cdin_kernel *k, cdin_doc d,
                                   const cdin_history_request *req, cdin_edit_result *out);
CDIN_API cdin_status cdin_doc_redo(cdin_kernel *k, cdin_doc d,
                                   const cdin_history_request *req, cdin_edit_result *out);

/* Saved-state marker, metadata, history counters (DOC-040/041, UNDO-030). */
CDIN_API cdin_status cdin_doc_mark_clean(cdin_kernel *k, cdin_doc d);
CDIN_API cdin_status cdin_doc_is_clean(cdin_kernel *k, cdin_doc d, int *out);
CDIN_API cdin_status cdin_doc_set_metadata(cdin_kernel *k, cdin_doc d, uint64_t metadata);
CDIN_API cdin_status cdin_doc_get_metadata(cdin_kernel *k, cdin_doc d, uint64_t *out);
CDIN_API cdin_status cdin_doc_history_info(cdin_kernel *k, cdin_doc d, uint64_t *undo_units,
                                           uint64_t *redo_units, uint64_t *evicted_total,
                                           uint64_t *memory_bytes);

/* ---- transactions (TXN-*) ------------------------------------------------- */
typedef struct cdin_txn_begin_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    uint64_t expected_version;
} cdin_txn_begin_request;

typedef struct cdin_txn_commit_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    const uint8_t *blob_before;
    size_t blob_before_len;
    const uint8_t *blob_after;
    size_t blob_after_len;
} cdin_txn_commit_request;

CDIN_API cdin_status cdin_txn_begin(cdin_kernel *k, cdin_doc d,
                                    const cdin_txn_begin_request *req, cdin_txn *out);
/* Positions are validated against tentative content. Set expected_version to
 * CDIN_ANY_VERSION, coalesce_key to 0, and both blob lengths to 0; commit owns
 * the optional before/after blobs. */
CDIN_API cdin_status cdin_txn_replace(cdin_kernel *k, cdin_txn t,
                                      const cdin_replace_request *req, cdin_edit_result *out);
CDIN_API cdin_status cdin_txn_commit(cdin_kernel *k, cdin_txn t,
                                     const cdin_txn_commit_request *req, cdin_edit_result *out);
CDIN_API cdin_status cdin_txn_abort(cdin_kernel *k, cdin_txn t);
/* Tentative content: only the transaction handle may read it (TXN-008). */
CDIN_API cdin_status cdin_txn_len(cdin_kernel *k, cdin_txn t, uint64_t *out);
CDIN_API cdin_status cdin_txn_read(cdin_kernel *k, cdin_txn t, uint64_t start, uint64_t end,
                                   uint8_t *buf, size_t bufsz, size_t *out_len);

/* ---- snapshots (CONC-005/006) --------------------------------------------- */
CDIN_API cdin_status cdin_snapshot_acquire(cdin_kernel *k, cdin_doc d,
                                           cdin_version *out_version, cdin_snapshot *out);
CDIN_API cdin_status cdin_snapshot_release(cdin_kernel *k, cdin_snapshot s);

/* ---- reading and positions (source = document or snapshot) ---------------- */
#define CDIN_READ_STARTS_MID_SEQUENCE 1u  /* range begins inside a multibyte sequence */
#define CDIN_READ_ENDS_MID_SEQUENCE   2u  /* range ends inside a multibyte sequence */
#define CDIN_CONV_TOUCHED_RAW         1u  /* conversion crossed raw bytes (POS-031) */

CDIN_API cdin_status cdin_text_version(cdin_kernel *k, cdin_source src, cdin_version *out);
CDIN_API cdin_status cdin_text_len(cdin_kernel *k, cdin_source src, uint64_t *out);
/* buf == NULL: *out_len receives the byte count. Too small: BUFFER_TOO_SMALL,
 * *out_len receives the required size. flags (optional) receives CDIN_READ_*. */
CDIN_API cdin_status cdin_text_read(cdin_kernel *k, cdin_source src, uint64_t start, uint64_t end,
                                    uint8_t *buf, size_t bufsz, size_t *out_len, uint32_t *flags);
CDIN_API cdin_status cdin_text_serialize(cdin_kernel *k, cdin_source src,
                                         uint8_t *buf, size_t bufsz, size_t *out_len);

/* Positions are valid for one version. expected_version may be CDIN_ANY_VERSION;
 * otherwise a mismatch fails with STALE_VERSION (POS-034). */
CDIN_API cdin_status cdin_text_check_position(cdin_kernel *k, cdin_source src, cdin_version expected_version,
                                              uint64_t offset); /* OK, OUT_OF_RANGE or INVALID_POSITION */
/* Region containing `offset` (offset < length): [start,end) and whether it is raw. */
CDIN_API cdin_status cdin_text_region_at(cdin_kernel *k, cdin_source src, uint64_t offset,
                                         uint64_t *start, uint64_t *end, int *is_raw);

CDIN_API cdin_status cdin_text_line_count(cdin_kernel *k, cdin_source src, uint64_t *out);
CDIN_API cdin_status cdin_text_line_start(cdin_kernel *k, cdin_source src, uint64_t line, uint64_t *out);
/* col is a byte offset within the line (POS-013). */
CDIN_API cdin_status cdin_text_line_from_offset(cdin_kernel *k, cdin_source src, uint64_t offset,
                                                uint64_t *line, uint64_t *col);

typedef struct cdin_eol_info {
    uint32_t struct_size;
    uint64_t lf, crlf, cr;        /* terminator counts */
    int final_line_unterminated;  /* last line has no terminator */
    int mixed;                    /* more than one style present */
} cdin_eol_info;
CDIN_API cdin_status cdin_text_eol_info(cdin_kernel *k, cdin_source src, cdin_eol_info *out);

CDIN_API cdin_status cdin_text_byte_to_codepoint(cdin_kernel *k, cdin_source src, uint64_t offset,
                                                 uint64_t *out, uint32_t *flags);
CDIN_API cdin_status cdin_text_codepoint_to_byte(cdin_kernel *k, cdin_source src, uint64_t index,
                                                 uint64_t *out, uint32_t *flags);
/* Next/previous code-point boundary strictly after/before offset. */
CDIN_API cdin_status cdin_text_codepoint_next(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);
CDIN_API cdin_status cdin_text_codepoint_prev(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);
/* Next/previous extended grapheme cluster boundary strictly after/before offset
 * (Unicode version of cdin_unicode_version). Raw bytes are one cluster each. */
CDIN_API cdin_status cdin_text_grapheme_next(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);
CDIN_API cdin_status cdin_text_grapheme_prev(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);

/* ---- notifications (NOTIF-*) ---------------------------------------------- */
typedef enum cdin_notif_kind {
    CDIN_NOTIF_EDIT = 0,
    CDIN_NOTIF_BATCH,
    CDIN_NOTIF_TRANSACTION,
    CDIN_NOTIF_UNDO,
    CDIN_NOTIF_REDO,
    CDIN_NOTIF_RELOAD,
    CDIN_NOTIF_CLOSED,
    CDIN_NOTIF_HISTORY_EVICTED,
    CDIN_NOTIF_RESYNC,
    CDIN_NOTIF_SAVED_STATE_CHANGED
} cdin_notif_kind;

#define CDIN_NOTIF_MAX_EDITS 8u

typedef struct cdin_edit_desc {
    uint64_t start;      /* in the coordinates of the state just before this edit */
    uint64_t removed;
    uint64_t inserted;
} cdin_edit_desc;

typedef struct cdin_notif_event {
    uint32_t struct_size;
    cdin_notif_kind kind;
    cdin_doc doc;
    cdin_version from_version;  /* to_version == from_version + 1 for edit-like kinds (NOTIF-011) */
    cdin_version to_version;
    cdin_version current_version; /* RESYNC: version to re-read at; CLOSED: last version */
    uint32_t edit_count;        /* total number of primitive edits */
    uint32_t edits_stored;      /* min(edit_count, CDIN_NOTIF_MAX_EDITS) */
    int edits_truncated;        /* edit_count > edits_stored: invalidate instead of replaying */
    uint64_t evicted;           /* HISTORY_EVICTED: units dropped by this event */
    int is_clean;               /* SAVED_STATE_CHANGED: new value */
    cdin_edit_desc edits[CDIN_NOTIF_MAX_EDITS];
} cdin_notif_event;

/* May only signal (ABI-016); runs on the thread that made the mutation, after
 * the kernel lock was released. */
typedef void (*cdin_notif_wakeup)(void *ctx);

typedef struct cdin_subscribe_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    cdin_notif_wakeup wakeup;     /* optional */
    void *wakeup_ctx;
} cdin_subscribe_request;

/* Returns the version the subscription starts at (NOTIF-009). */
CDIN_API cdin_status cdin_notif_subscribe(cdin_kernel *k, cdin_doc d, const cdin_subscribe_request *req,
                                          cdin_version *out_version, cdin_notif *out);
CDIN_API cdin_status cdin_notif_unsubscribe(cdin_kernel *k, cdin_notif n);
CDIN_API cdin_status cdin_notif_drain(cdin_kernel *k, cdin_notif n, cdin_notif_event *events,
                                      size_t max, size_t *out_count);

/* ---- search (SEARCH-*) ----------------------------------------------------- */
typedef enum cdin_search_mode {
    CDIN_SEARCH_LITERAL = 0,  /* pattern is UTF-8 text; matches lie in valid regions */
    CDIN_SEARCH_REGEX = 1,    /* not available in this release: UNSUPPORTED */
    CDIN_SEARCH_BYTES = 2     /* byte-exact, anywhere */
} cdin_search_mode;

typedef enum cdin_search_direction {
    CDIN_SEARCH_FORWARD = 0,
    CDIN_SEARCH_BACKWARD = 1
} cdin_search_direction;

#define CDIN_SEARCH_CASE_INSENSITIVE 1u   /* simple Unicode case folding (literal mode) */
#define CDIN_SEARCH_ANY_NEWLINE      2u   /* LF in the pattern matches LF, CRLF or CR (SEARCH-032) */
#define CDIN_SEARCH_WANT_PARTIAL     4u   /* on LIMIT_EXCEEDED/CANCELLED keep matches found so far */

typedef struct cdin_search_request {
    uint32_t struct_size;
    uint32_t flags;
    cdin_source src;
    cdin_version expected_version;   /* or CDIN_ANY_VERSION */
    const uint8_t *pattern;
    size_t pattern_len;
    cdin_search_mode mode;
    cdin_search_direction direction;
    /* FORWARD: matches start at >= from and end <= bound.
     * BACKWARD: matches start at <= from (inclusive) and end <= bound.
     * bound == UINT64_MAX means the end of the text. Matches never overlap. */
    uint64_t from;
    uint64_t bound;
    uint64_t limit;                  /* max matches returned (<= ranges_capacity) */
    uint64_t step_limit;             /* 0 = kernel default; a step is one candidate start examined */
    const uint32_t *cancel;          /* optional; non-zero (atomic relaxed load) cancels */
    uint64_t *ranges;                /* out: [start,end) pairs */
    size_t ranges_capacity;          /* in pairs */
} cdin_search_request;

typedef struct cdin_search_result {
    uint32_t struct_size;
    cdin_version version;
    uint64_t count;
    int truncated;                   /* stopped because `limit` matches were found */
    int partial;                     /* stopped early (limit/cancel) with WANT_PARTIAL */
    int skipped_raw;                 /* literal mode: the text has raw regions that cannot match (SEARCH-042) */
    uint64_t next_from;              /* pass as `from` to continue */
    uint64_t next_bound;             /* pass as `bound` to continue (BACKWARD) */
    int exhausted;                   /* nothing left to continue with */
    uint64_t steps;
    cdin_error_details details;
} cdin_search_result;

CDIN_API cdin_status cdin_search(cdin_kernel *k, const cdin_search_request *req, cdin_search_result *out);

#ifdef __cplusplus
}
#endif

#endif /* CDIN_KERNEL_H */
