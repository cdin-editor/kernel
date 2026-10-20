/**
 * @file kernel.h
 * @brief Public C API for the cdin-kernel text engine.
 *
 * This is the single public header. The library exposes a versioned C ABI;
 * use this header from both C and C++ applications.
 *
 * General API rules:
 * - Every API call returns a @ref cdin_status. Results are returned through
 *   output parameters rather than through function return values.
 * - Set `struct_size` to `sizeof(the_struct)` on each input or output
 *   structure that defines this field.
 * - The library does not transfer its internal buffers to the caller. Text
 *   and other variable-length data are copied into caller-owned buffers.
 *   Pass a NULL output buffer where documented to query the required size.
 * - Failed calls leave output values unchanged unless the function explicitly
 *   documents an error-details output.
 * - A zero-valued configurable limit selects that limit's documented default.
 *
 * Thread safety:
 * - With a valid lock/unlock pair configured in `cdin_kernel_config`, API
 *   calls may be made from different threads, subject to each function's
 *   lifetime rules.
 * - Without a lock pair, use a kernel from one thread at a time.
 * - `cdin_kernel_destroy()` must never run concurrently with another call;
 *   no handle may be used after the kernel is destroyed.
 * - Log and wakeup callbacks are invoked without the kernel lock held.
 *
 * ABI compatibility: numeric status/enum values, public structure layouts,
 * and function signatures are fixed for ABI major version 1.
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

#define CDIN_KERNEL_ABI_MAJOR 1u
#define CDIN_KERNEL_ABI_MINOR 0u
#define CDIN_KERNEL_ABI_PATCH 0u
#define CDIN_KERNEL_SPEC_VERSION_MAJOR 1u
#define CDIN_KERNEL_SPEC_VERSION_MINOR 0u
#define CDIN_KERNEL_SPEC_VERSION_PATCH 0u

/**
 * @brief Result codes returned by public API functions.
 *
 * The underlying type is `uint32_t` so its width is stable across compilers.
 * Numeric values are part of ABI major version 1 and must not be renumbered.
 */
typedef uint32_t cdin_status;
enum {
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
};

/** Return the static, NUL-terminated name of a status code. */
CDIN_API const char *cdin_status_name(cdin_status s);

/* --------------------------------------------------------------------------
 * Handles and text sources
 * -------------------------------------------------------------------------- */
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

/**
 * @brief Identifies a document or an immutable snapshot for read operations.
 *
 * Set exactly one of `doc` or `snapshot`. Use `cdin_src_doc()` or
 * `cdin_src_snap()` to construct a source without manually initializing it.
 */
typedef struct cdin_source {
    cdin_doc doc;
    cdin_snapshot snapshot;
} cdin_source;

static inline cdin_source cdin_src_doc(cdin_doc doc)
{
    cdin_source source = { doc, 0 };
    return source;
}

static inline cdin_source cdin_src_snap(cdin_snapshot snapshot)
{
    cdin_source source = { 0, snapshot };
    return source;
}

/* --------------------------------------------------------------------------
 * Caller-provided services
 * -------------------------------------------------------------------------- */
/**
 * @brief Allocation callbacks used for all memory owned by a kernel.
 *
 * `alloc` receives a size of at least one byte. `realloc` receives the exact
 * old and new allocation sizes, and `free` receives the exact current size.
 * All three callbacks and their context must remain valid until destruction.
 */
typedef struct cdin_allocator {
    uint32_t struct_size;
    void *(*alloc)(void *ctx, size_t n);                           /* n >= 1 */
    void *(*realloc)(void *ctx, void *p, size_t old_n, size_t new_n); /* p != NULL, sizes >= 1 */
    void (*free)(void *ctx, void *p, size_t n);                    /* n is the exact current size */
    void *ctx;
} cdin_allocator;

/**
 * @brief Optional lock callbacks used to serialize access to a kernel.
 *
 * Provide both `lock` and `unlock`, or leave both NULL. When omitted, the
 * caller must not use the same kernel concurrently from multiple threads.
 */
typedef struct cdin_lock_vtable {
    uint32_t struct_size;
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} cdin_lock_vtable;

/** Optional log callback for diagnostic messages produced by the kernel. */
typedef struct cdin_log_sink {
    uint32_t struct_size;
    void *ctx;
    /* level: 0 = error, 1 = warning, 2 = info. `msg` is UTF-8 with explicit length. */
    void (*log)(void *ctx, int level, const char *msg, size_t len);
} cdin_log_sink;

/** Limit identifiers reported in `cdin_error_details.limit_id`. */
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
    CDIN_LIMIT_TXN_MEMORY = 9,
    CDIN_LIMIT_BATCH_EDITS = 10,
    CDIN_LIMIT_VERSION = 11,
    CDIN_LIMIT_SEQUENCE = 12
};

/** Default limits and hard caps used by the library. */
#define CDIN_DEFAULT_MAX_DOCUMENTS        1024u
#define CDIN_DEFAULT_MAX_DOCUMENT_SIZE    ((uint64_t)1 << 30)
#define CDIN_DEFAULT_MAX_UNDO_MEMORY      ((uint64_t)64 << 20)
#define CDIN_MAX_BATCH_EDITS_HARD         65536u
#define CDIN_DEFAULT_MAX_BLOB_SIZE        4096u
#define CDIN_DEFAULT_MAX_SNAPSHOTS        16u
#define CDIN_DEFAULT_MAX_SUBSCRIBERS      16u
#define CDIN_MAX_SUBSCRIBERS_HARD         64u
#define CDIN_DEFAULT_NOTIF_QUEUE          64u
#define CDIN_DEFAULT_SEARCH_STEPS         ((uint64_t)1 << 32)

/**
 * @brief Configuration used when creating a kernel.
 *
 * Set `struct_size`, `abi_major`, and the allocator callbacks. Initialize the
 * structure to zero first so reserved fields and default limits stay zero.
 * The lock and log services are optional. A zero limit selects its documented
 * default; `max_edit_size == 0` follows `max_document_size`.
 */
typedef struct cdin_kernel_config {
    uint32_t struct_size;
    uint32_t abi_major;              /* set to CDIN_KERNEL_ABI_MAJOR */
    cdin_allocator allocator;
    cdin_lock_vtable lock;           /* optional */
    cdin_log_sink log;               /* optional */
    uint64_t max_documents;          /* live documents */
    uint64_t max_document_size;      /* bytes */
    uint64_t max_edit_size;          /* inserted bytes per edit; 0 follows max_document_size */
    uint64_t max_undo_memory;        /* bytes, per document */
    uint64_t max_blob_size;          /* bytes, per blob */
    uint64_t max_snapshots_per_doc;
    uint64_t max_subscribers_per_doc;
    uint64_t notif_queue_capacity;   /* queued events per subscriber */
    uint64_t default_search_steps;   /* used when a request sets step_limit to 0 */
} cdin_kernel_config;

/* --------------------------------------------------------------------------
 * Error details
 * -------------------------------------------------------------------------- */
/**
 * @brief Additional details for a failed call, where that API provides them.
 *
 * Fields are meaningful only for the corresponding status (for example,
 * `expected` and `current` for `CDIN_STALE_VERSION`). Callers should check the
 * status before reading these fields.
 */
typedef struct cdin_error_details {
    uint32_t struct_size;
    uint32_t limit_id;     /* LIMIT_EXCEEDED: which CDIN_LIMIT_* */
    uint64_t expected;     /* STALE_VERSION */
    uint64_t current;      /* STALE_VERSION */
    uint64_t requested;    /* OUT_OF_RANGE / INVALID_POSITION: offending value */
    uint64_t length;       /* OUT_OF_RANGE: document length */
    uint64_t required;     /* BUFFER_TOO_SMALL: bytes needed */
} cdin_error_details;

/* --------------------------------------------------------------------------
 * Version and feature queries
 * -------------------------------------------------------------------------- */
#define CDIN_FEAT_BATCH          (1ull << 0)
#define CDIN_FEAT_TRANSACTIONS   (1ull << 1)
#define CDIN_FEAT_SNAPSHOTS      (1ull << 2)
#define CDIN_FEAT_NOTIFICATIONS  (1ull << 3)
#define CDIN_FEAT_SEARCH_LITERAL (1ull << 4)
#define CDIN_FEAT_SEARCH_BYTES   (1ull << 5)
#define CDIN_FEAT_SEARCH_REGEX   (1ull << 6)   /* NOT set in this release */
#define CDIN_FEAT_GRAPHEME       (1ull << 7)
#define CDIN_FEAT_CASEFOLD_SIMPLE (1ull << 8)
#define CDIN_FEAT_LOCKING        (1ull << 9)   /* caller-provided lock vtable is supported */

/** Return the C ABI version implemented by the loaded library. */
CDIN_API cdin_status cdin_abi_version(uint32_t *major, uint32_t *minor, uint32_t *patch);
/** Return the feature bit mask supported by this build. */
CDIN_API cdin_status cdin_features(uint64_t *mask);
/** Return the Unicode data version used by text-position helpers. */
CDIN_API cdin_status cdin_unicode_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/* --------------------------------------------------------------------------
 * Kernel lifecycle
 * -------------------------------------------------------------------------- */
/**
 * Create a kernel using the supplied configuration and allocator callbacks.
 * Set `struct_size` on every nested service structure and set `abi_major` to
 * `CDIN_KERNEL_ABI_MAJOR`.
 */
CDIN_API cdin_status cdin_kernel_create(const cdin_kernel_config *cfg, cdin_kernel **out);
/**
 * Destroy a kernel and release all resources it owns.
 *
 * This call invalidates all document, transaction, snapshot, and notification
 * handles issued by the kernel. It must not overlap any other API call.
 */
CDIN_API cdin_status cdin_kernel_destroy(cdin_kernel *k);

typedef uint32_t cdin_shutdown_mode;
enum {
    CDIN_SHUTDOWN_GRACEFUL = 0,
    CDIN_SHUTDOWN_ABORT = 1
};

/**
 * Stop accepting edits and abort any open transactions.
 *
 * Documents remain readable and can still be closed. Snapshot reads/releases
 * and notification drain/unsubscribe calls also remain available. Calls are
 * synchronous; both modes have the same behavior in ABI 1.0.0. Destroy the
 * kernel to release any state that remains afterward.
 */
CDIN_API cdin_status cdin_kernel_shutdown(cdin_kernel *k, cdin_shutdown_mode mode);

/** Return the sequence number assigned to the most recently accepted mutation. */
CDIN_API cdin_status cdin_kernel_last_sequence(cdin_kernel *k, uint64_t *out);

/* --------------------------------------------------------------------------
 * Documents and reload requests
 * -------------------------------------------------------------------------- */
/** Create a document by copying `len` bytes from `bytes`. The initial document version is 0. */
CDIN_API cdin_status cdin_doc_create(cdin_kernel *k, const uint8_t *bytes, size_t len, cdin_doc *out);
/** Close a document, abort its active transaction, and emit a closed event. */
CDIN_API cdin_status cdin_doc_close(cdin_kernel *k, cdin_doc d);

/** Request structure for replacing a document's entire contents. */
typedef struct cdin_reload_request {
    uint32_t struct_size;
    uint32_t flags;               /* must be 0; preserving history is not supported */
    uint64_t expected_version;
    const uint8_t *bytes;
    size_t len;
} cdin_reload_request;
#define CDIN_RELOAD_KEEP_HISTORY 1u

/* --------------------------------------------------------------------------
 * Edits and edit results
 * -------------------------------------------------------------------------- */
/**
 * @brief Request to replace one byte range in a document or transaction.
 *
 * The range is half-open: `[start, end)`. The kernel copies input bytes before
 * returning, so the caller may release the input buffer after the call.
 */
typedef struct cdin_replace_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved, must be 0 */
    uint64_t expected_version;    /* CDIN_ANY_VERSION disables the version check */
    uint64_t start, end;          /* half-open byte range: [start, end) */
    const uint8_t *bytes;
    size_t len;
    uint64_t coalesce_key;        /* 0 disables edit coalescing */
    const uint8_t *blob_before;   /* opaque caller state restored by undo */
    size_t blob_before_len;
    const uint8_t *blob_after;    /* ... restored on redo */
    size_t blob_after_len;
} cdin_replace_request;

/** Result of an edit, undo, redo, reload, or batch operation. */
typedef struct cdin_edit_result {
    uint32_t struct_size;         /* set by the caller to sizeof */
    uint32_t edit_count;          /* number of primitive edits applied */
    cdin_version new_version;     /* equals the old version for a no-op */
    uint64_t accepted_seq;        /* zero means the mutation was not accepted */
    /* Valid only when edit_count == 1 (otherwise 0); coordinates of the
     * previous version. Multi-edit changes are described by notifications. */
    uint64_t start, removed, inserted;
    uint64_t new_length;
    uint64_t blob_len;            /* size of the caller-state blob for this history unit */
    cdin_error_details details;   /* written on failure */
} cdin_edit_result;

/** Replace the half-open byte range `[start, end)` if `expected_version` matches the document version. */
CDIN_API cdin_status cdin_doc_replace(cdin_kernel *k, cdin_doc d,
                                      const cdin_replace_request *req, cdin_edit_result *out);
/** Replace the document contents with the supplied bytes, subject to the expected-version check. */
CDIN_API cdin_status cdin_doc_reload(cdin_kernel *k, cdin_doc d,
                                     const cdin_reload_request *req, cdin_edit_result *out);

/** A set of non-overlapping edits that all apply to one document version. */
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

/** Apply sorted, non-overlapping replacements as one undoable operation. */
CDIN_API cdin_status cdin_doc_replace_batch(cdin_kernel *k, cdin_doc d,
                                            const cdin_batch_request *req, cdin_edit_result *out);

/* --------------------------------------------------------------------------
 * Undo, redo, and document state
 * -------------------------------------------------------------------------- */
/** Request options for undo and redo operations. */
typedef struct cdin_history_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    uint64_t expected_version;
    uint8_t *blob_out;            /* optional: receives the unit's blob (before on undo, after on redo) */
    size_t blob_capacity;
} cdin_history_request;

/** Undo the most recent undo unit if the expected document version matches. */
CDIN_API cdin_status cdin_doc_undo(cdin_kernel *k, cdin_doc d,
                                   const cdin_history_request *req, cdin_edit_result *out);
/** Redo the next undo unit if the expected document version matches. */
CDIN_API cdin_status cdin_doc_redo(cdin_kernel *k, cdin_doc d,
                                   const cdin_history_request *req, cdin_edit_result *out);

/** Saved-state marker, caller metadata, and undo/redo history statistics. */
CDIN_API cdin_status cdin_doc_mark_clean(cdin_kernel *k, cdin_doc d);
CDIN_API cdin_status cdin_doc_is_clean(cdin_kernel *k, cdin_doc d, int *out);
CDIN_API cdin_status cdin_doc_set_metadata(cdin_kernel *k, cdin_doc d, uint64_t metadata);
CDIN_API cdin_status cdin_doc_get_metadata(cdin_kernel *k, cdin_doc d, uint64_t *out);
CDIN_API cdin_status cdin_doc_history_info(cdin_kernel *k, cdin_doc d, uint64_t *undo_units,
                                           uint64_t *redo_units, uint64_t *evicted_total,
                                           uint64_t *memory_bytes);

/* --------------------------------------------------------------------------
 * Transactions
 * -------------------------------------------------------------------------- */
/** Options for beginning a transaction against a specific document version. */
typedef struct cdin_txn_begin_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    uint64_t expected_version;
} cdin_txn_begin_request;

/** Caller state to associate with the transaction's resulting undo unit. */
typedef struct cdin_txn_commit_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    const uint8_t *blob_before;
    size_t blob_before_len;
    const uint8_t *blob_after;
    size_t blob_after_len;
} cdin_txn_commit_request;

/** Begin a transaction against a document version. Only one transaction may be active per document. */
CDIN_API cdin_status cdin_txn_begin(cdin_kernel *k, cdin_doc d,
                                    const cdin_txn_begin_request *req, cdin_txn *out);
/**
 * Apply an edit to tentative transaction content.
 *
 * The edit is not visible to document readers until commit. For this request,
 * set `expected_version` to `CDIN_ANY_VERSION`, `coalesce_key` to 0, and both
 * history-blob lengths to 0. Pass optional before/after blobs to commit instead.
 */
CDIN_API cdin_status cdin_txn_replace(cdin_kernel *k, cdin_txn t,
                                      const cdin_replace_request *req, cdin_edit_result *out);
/** Publish all edits in a transaction as a single document mutation and undo unit. */
CDIN_API cdin_status cdin_txn_commit(cdin_kernel *k, cdin_txn t,
                                     const cdin_txn_commit_request *req, cdin_edit_result *out);
/** Discard a transaction without publishing its tentative content. */
CDIN_API cdin_status cdin_txn_abort(cdin_kernel *k, cdin_txn t);
/** Read the transaction's tentative content; ordinary document reads do not see it. */
CDIN_API cdin_status cdin_txn_len(cdin_kernel *k, cdin_txn t, uint64_t *out);
/** Copy a byte range from the transaction's tentative content into caller memory. */
CDIN_API cdin_status cdin_txn_read(cdin_kernel *k, cdin_txn t, uint64_t start, uint64_t end,
                                   uint8_t *buf, size_t bufsz, size_t *out_len);

/* --------------------------------------------------------------------------
 * Immutable snapshots
 * -------------------------------------------------------------------------- */
/** Create an immutable snapshot and return the document version it captures. */
CDIN_API cdin_status cdin_snapshot_acquire(cdin_kernel *k, cdin_doc d,
                                           cdin_version *out_version, cdin_snapshot *out);
/** Release a snapshot when the caller no longer needs its stable view of the text. */
CDIN_API cdin_status cdin_snapshot_release(cdin_kernel *k, cdin_snapshot s);

/* --------------------------------------------------------------------------
 * Reading text and converting positions
 * -------------------------------------------------------------------------- */
#define CDIN_READ_STARTS_MID_SEQUENCE 1u  /* range begins inside a multibyte sequence */
#define CDIN_READ_ENDS_MID_SEQUENCE   2u  /* range ends inside a multibyte sequence */
#define CDIN_CONV_TOUCHED_RAW         1u  /* conversion crossed invalid/raw UTF-8 bytes */

/** Return the version of a document or snapshot. */
CDIN_API cdin_status cdin_text_version(cdin_kernel *k, cdin_source src, cdin_version *out);
/** Return the source length in bytes. */
CDIN_API cdin_status cdin_text_len(cdin_kernel *k, cdin_source src, uint64_t *out);
/**
 * Copy bytes from `[start, end)` into a caller-owned buffer.
 *
 * If `buf` is NULL, `*out_len` receives the required byte count. If the buffer
 * is too small, the function returns `CDIN_BUFFER_TOO_SMALL` and reports the
 * required size in `*out_len`. `flags` may report a range boundary inside a
 * multi-byte UTF-8 sequence.
 */
CDIN_API cdin_status cdin_text_read(cdin_kernel *k, cdin_source src, uint64_t start, uint64_t end,
                                    uint8_t *buf, size_t bufsz, size_t *out_len, uint32_t *flags);
/** Copy the entire source into a caller-owned buffer using the same size-query pattern as `cdin_text_read()`. */
CDIN_API cdin_status cdin_text_serialize(cdin_kernel *k, cdin_source src,
                                         uint8_t *buf, size_t bufsz, size_t *out_len);

/** Validate a byte offset against a source and, optionally, an expected version. */
CDIN_API cdin_status cdin_text_check_position(cdin_kernel *k, cdin_source src, cdin_version expected_version,
                                              uint64_t offset); /* OK, OUT_OF_RANGE or INVALID_POSITION */
/** Return the UTF-8 region containing `offset`; `offset` must be before end-of-text. */
CDIN_API cdin_status cdin_text_region_at(cdin_kernel *k, cdin_source src, uint64_t offset,
                                         uint64_t *start, uint64_t *end, int *is_raw);

/** Return the number of lines, including a final unterminated line when present. */
CDIN_API cdin_status cdin_text_line_count(cdin_kernel *k, cdin_source src, uint64_t *out);
/** Return the byte offset where a zero-based line begins. */
CDIN_API cdin_status cdin_text_line_start(cdin_kernel *k, cdin_source src, uint64_t line, uint64_t *out);
/** Convert a byte offset to a zero-based line index and byte column. */
CDIN_API cdin_status cdin_text_line_from_offset(cdin_kernel *k, cdin_source src, uint64_t offset,
                                                uint64_t *line, uint64_t *col);

/** Line-ending counts for a text source; counts describe terminators, not lines. */
typedef struct cdin_eol_info {
    uint32_t struct_size;
    uint64_t lf, crlf, cr;        /* terminator counts */
    int final_line_unterminated;  /* last line has no terminator */
    int mixed;                    /* more than one style present */
} cdin_eol_info;
/** Count LF, CRLF, and CR line terminators without modifying the text. */
CDIN_API cdin_status cdin_text_eol_info(cdin_kernel *k, cdin_source src, cdin_eol_info *out);

/** Convert a valid byte position to a zero-based code-point index. */
CDIN_API cdin_status cdin_text_byte_to_codepoint(cdin_kernel *k, cdin_source src, uint64_t offset,
                                                 uint64_t *out, uint32_t *flags);
/** Convert a zero-based code-point index to a byte offset. */
CDIN_API cdin_status cdin_text_codepoint_to_byte(cdin_kernel *k, cdin_source src, uint64_t index,
                                                 uint64_t *out, uint32_t *flags);
/** Return the next or previous Unicode code-point boundary from a byte offset. */
CDIN_API cdin_status cdin_text_codepoint_next(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);
CDIN_API cdin_status cdin_text_codepoint_prev(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);
/**
 * Return the next or previous extended grapheme-cluster boundary.
 *
 * Segmentation follows the Unicode data version returned by
 * `cdin_unicode_version()`. Each byte in a raw/invalid UTF-8 region is treated
 * as an individual cluster.
 */
CDIN_API cdin_status cdin_text_grapheme_next(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);
CDIN_API cdin_status cdin_text_grapheme_prev(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out);

/* --------------------------------------------------------------------------
 * Change notifications
 * -------------------------------------------------------------------------- */
typedef uint32_t cdin_notif_kind;
enum {
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
};

#define CDIN_NOTIF_MAX_EDITS 8u

typedef struct cdin_edit_desc {
    uint64_t start;      /* in the coordinates of the state just before this edit */
    uint64_t removed;
    uint64_t inserted;
} cdin_edit_desc;

/**
 * @brief One queued document-change event.
 *
 * Event kinds describe the mutation or state change. For edits, the recorded
 * ranges use byte offsets from the state immediately before the change. If
 * `edits_truncated` is nonzero, refresh from the source rather than replaying
 * the incomplete edit list.
 */
typedef struct cdin_notif_event {
    uint32_t struct_size;
    cdin_notif_kind kind;
    cdin_doc doc;
    cdin_version from_version;  /* edit-like events advance by one version */
    cdin_version to_version;
    cdin_version current_version; /* RESYNC: version to re-read at; CLOSED: last version */
    uint32_t edit_count;        /* total number of primitive edits */
    uint32_t edits_stored;      /* min(edit_count, CDIN_NOTIF_MAX_EDITS) */
    int edits_truncated;        /* edit_count > edits_stored: invalidate instead of replaying */
    uint64_t evicted;           /* history units dropped in a history-eviction event */
    int is_clean;               /* SAVED_STATE_CHANGED: new value */
    cdin_edit_desc edits[CDIN_NOTIF_MAX_EDITS];
} cdin_notif_event;

/**
 * Optional notification callback. It should only wake a consumer; drain events
 * separately. It runs on the mutating thread after the kernel lock is released.
 */
typedef void (*cdin_notif_wakeup)(void *ctx);

/** Options for a document notification subscription. */
typedef struct cdin_subscribe_request {
    uint32_t struct_size;
    uint32_t flags;               /* reserved */
    cdin_notif_wakeup wakeup;     /* optional */
    void *wakeup_ctx;
} cdin_subscribe_request;

/** Subscribe to a document and return the version at which the subscription begins. */
CDIN_API cdin_status cdin_notif_subscribe(cdin_kernel *k, cdin_doc d, const cdin_subscribe_request *req,
                                          cdin_version *out_version, cdin_notif *out);
/** Stop a subscription and release its queued notification storage. */
CDIN_API cdin_status cdin_notif_unsubscribe(cdin_kernel *k, cdin_notif n);
/** Copy queued notification events into the caller-provided array and report how many were written. */
CDIN_API cdin_status cdin_notif_drain(cdin_kernel *k, cdin_notif n, cdin_notif_event *events,
                                      size_t max, size_t *out_count);

/* --------------------------------------------------------------------------
 * Search
 * -------------------------------------------------------------------------- */
typedef uint32_t cdin_search_mode;
enum {
    CDIN_SEARCH_LITERAL = 0,  /* pattern is UTF-8 text; matches lie in valid regions */
    CDIN_SEARCH_REGEX = 1,    /* not available in this release: UNSUPPORTED */
    CDIN_SEARCH_BYTES = 2     /* byte-exact, anywhere */
};

typedef uint32_t cdin_search_direction;
enum {
    CDIN_SEARCH_FORWARD = 0,
    CDIN_SEARCH_BACKWARD = 1
};

#define CDIN_SEARCH_CASE_INSENSITIVE 1u   /* simple Unicode case folding (literal mode) */
#define CDIN_SEARCH_ANY_NEWLINE      2u   /* LF in a pattern matches LF, CRLF, or CR */
#define CDIN_SEARCH_WANT_PARTIAL     4u   /* on LIMIT_EXCEEDED/CANCELLED keep matches found so far */

/**
 * @brief Search configuration and caller-owned result storage.
 *
 * The result array stores pairs: `start, end, start, end, ...`. Its capacity is
 * measured in pairs, not integers. Set `limit` no greater than
 * `ranges_capacity`. Offsets are byte offsets into the selected source.
 */
typedef struct cdin_search_request {
    uint32_t struct_size;
    uint32_t flags;
    cdin_source src;
    cdin_version expected_version;   /* or CDIN_ANY_VERSION */
    const uint8_t *pattern;
    size_t pattern_len;
    cdin_search_mode mode;
    cdin_search_direction direction;
    /* Forward: start >= from and end <= bound. Backward: start <= from and end <= bound.
     * UINT64_MAX means end-of-text. Returned matches do not overlap. */
    uint64_t from;
    uint64_t bound;
    uint64_t limit;                  /* maximum matches; must not exceed ranges_capacity */
    uint64_t step_limit;             /* 0 = kernel default; each step checks one candidate offset */
    const uint32_t *cancel;          /* optional; non-zero (atomic relaxed load) cancels */
    uint64_t *ranges;                /* out: [start,end) pairs */
    size_t ranges_capacity;          /* in pairs */
} cdin_search_request;

/** Search results and continuation information for a `cdin_search()` call. */
typedef struct cdin_search_result {
    uint32_t struct_size;
    cdin_version version;
    uint64_t count;
    int truncated;                   /* stopped because `limit` matches were found */
    int partial;                     /* stopped early (limit/cancel) with WANT_PARTIAL */
    int skipped_raw;                 /* literal search encountered invalid/raw UTF-8 regions */
    uint64_t next_from;              /* pass as `from` to continue */
    uint64_t next_bound;             /* pass as `bound` to continue (BACKWARD) */
    int exhausted;                   /* nothing left to continue with */
    uint64_t steps;
    cdin_error_details details;
} cdin_search_result;

/** Search a document or snapshot. Matches are returned as caller-owned `[start, end)` byte-offset pairs. */
CDIN_API cdin_status cdin_search(cdin_kernel *k, const cdin_search_request *req, cdin_search_result *out);

#ifdef __cplusplus
}
#endif

#endif /* CDIN_KERNEL_H */
