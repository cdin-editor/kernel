/**
 * @file internal.h
 * @brief Private declarations shared by cdin-kernel implementation modules.
 *
 * Do not include this header from applications. The supported public interface
 * is `include/cdin/kernel.h`.
 *
 * Ownership and locking:
 * - Mutable kernel state belongs to `struct cdin_kernel`.
 * - Public entry points normally hold the kernel lock for their full operation.
 *   The search implementation may release it while scanning a pinned,
 *   reference-counted immutable text block.
 * - A text block is modified in place only when its reference count is one;
 *   otherwise edits use copy-on-write.
 * - Wakeup and logging callbacks run after the kernel lock is released.
 *
 * Source layout:
 * - `util/`: allocation helpers, checked sizes, UTF-8, and Unicode tables.
 * - `core/`: reference-counted text blocks and reusable handle tables.
 * - `text/`: line indexing, position conversion, and grapheme boundaries.
 * - `search/`: literal/byte search and case folding.
 * - `history/`: undo records, limits, eviction, and clean-state tracking.
 * - `doc/`, `txn/`, `notif/`: document, transaction, and notification state.
 * - `api/`: public entry-point wrappers grouped by API family.
 * - `kernel.c`: kernel creation, destruction, shutdown, and version queries.
 */
#ifndef CDIN_INTERNAL_H
#define CDIN_INTERNAL_H

#include <cdin/kernel.h>
#include "util/unicode_tables.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- handles ---------------------------------------------------------------- */
#define HANDLE_IDX_BITS 24u
#define HANDLE_IDX_MASK ((1ull << HANDLE_IDX_BITS) - 1)
#define HANDLE_GEN_MAX  ((1ull << (64u - HANDLE_IDX_BITS)) - 1)

typedef struct pool_slot {
    void *item;          /* NULL when free */
    uint64_t gen;        /* >= 1; bumped on release, slot retired at HANDLE_GEN_MAX */
    uint32_t next_free;  /* index + 1, 0 = end of free list */
    uint8_t retired;
} pool_slot;

typedef struct pool {
    pool_slot *slots;
    size_t cap;
    size_t used;         /* high-water mark of handed-out slots */
    size_t live;
    uint32_t free_head;  /* index + 1 */
} pool;

/* ---- text block --------------------------------------------------------------- */
typedef struct text {
    uint32_t refs;
    size_t len;
    size_t cap;
    uint64_t *lines;     /* cached line starts (valid iff lines_valid) */
    size_t nlines;
    size_t lines_cap;
    int lines_valid;
    /* Optional exact 24-bit trigram-presence index. State: 0 absent, 1 building, 2 ready.
     * The block is pinned while a builder runs, so edits use copy-on-write. */
    uint8_t *trigram_bits;
    uint32_t trigram_state;
    uint32_t utf8_has_raw; /* valid only when trigram_state == 2 */
    uint8_t data[];      /* cap bytes */
} text;

#define TEXT_TRIGRAM_INDEX_BYTES ((size_t)1u << 21) /* 2^24 trigrams / 8 bits */
#define TEXT_SEARCH_INDEX_MIN_BYTES ((size_t)8u * 1024u * 1024u)

/* ---- history ------------------------------------------------------------------- */
typedef struct edit_rec {
    uint64_t start;          /* coordinates of the state just before this edit */
    uint8_t *removed;
    size_t removed_len;
    uint8_t *inserted;
    size_t inserted_len;
} edit_rec;

typedef struct unit {
    uint64_t id;
    edit_rec *edits;         /* in order of application */
    size_t n, cap;
    uint64_t key;
    uint8_t *blob_before;    size_t blob_before_len;
    uint8_t *blob_after;     size_t blob_after_len;
    size_t bytes;            /* accounted memory */
} unit;

typedef struct txn_state {
    uint64_t handle;         /* cdin_txn */
    cdin_doc doc;
    text *work;              /* private tentative content (shares the document's block until the first edit) */
    edit_rec *edits;
    size_t n, cap;
    size_t bytes;
    uint64_t serial;
} txn_state;

typedef struct doc {
    cdin_doc handle;
    text *txt;
    cdin_version version;
    unit *units;
    size_t nunits, units_cap;
    size_t head;             /* units [0, head) are applied */
    size_t undo_bytes;
    uint64_t evicted_total;
    uint64_t next_unit_id;
    uint64_t marker_id;
    int marker_lost;
    uint64_t coalesce_unit;  /* id of the unit still open for merging, 0 = none */
    uint64_t metadata;
    txn_state *tx;
    size_t nsnaps;
    size_t nsubs;
} doc;

typedef struct snapshot {
    text *txt;
    cdin_version version;
    cdin_doc doc;            /* handle of the document it was taken from */
} snapshot;

typedef struct subscriber {
    cdin_doc doc;
    int doc_closed;
    cdin_notif_wakeup wakeup;
    void *wakeup_ctx;
    cdin_notif_event *ring;
    size_t cap, head, count;
} subscriber;

/* ---- kernel -------------------------------------------------------------------- */
struct cdin_kernel {
    cdin_allocator al;
    cdin_lock_vtable lock;
    cdin_log_sink logs;
    uint64_t max_documents, max_document_size, max_edit_size, max_undo_memory;
    uint64_t max_blob_size, max_snapshots, max_subscribers, notif_capacity, default_steps;
    int stopped;
    uint64_t seq;
    uint64_t txn_serial;
    pool docs, snaps, subs, txns;
};

#define WAKE_MAX CDIN_MAX_SUBSCRIBERS_HARD
typedef struct wake_list {
    void (*fn[WAKE_MAX])(void *);
    void *ctx[WAKE_MAX];
    size_t n;
} wake_list;

/* ---- util/mem.c ---------------------------------------------------------------- */
/* These take the kernel because the allocator is injected per kernel. */
void *k_alloc(cdin_kernel *k, size_t n);                 /* n >= 1 */
void *k_realloc(cdin_kernel *k, void *p, size_t old_n, size_t new_n);
void k_free(cdin_kernel *k, void *p, size_t n);          /* p may be NULL */
/* Copy of src[0..len); returns NULL for len == 0. */
uint8_t *k_dup(cdin_kernel *k, const uint8_t *src, size_t len);
/* Checked size arithmetic; returns 1 on success, 0 on overflow, *out untouched. */
int k_add(size_t a, size_t b, size_t *out);
int k_mul(size_t a, size_t b, size_t *out);

/* ---- kernel.c and core.c ------------------------------------------------------- */
void k_lock(cdin_kernel *k);
void k_unlock(cdin_kernel *k);
void k_log(cdin_kernel *k, int level, const char *msg);
/* Validates a caller struct: declared size >= known, bytes beyond known are zero. */
cdin_status check_struct(const void *p, size_t known_size, size_t min_size);

cdin_status pool_insert(cdin_kernel *k, pool *pl, void *item, uint64_t *handle);
void *pool_get(const pool *pl, uint64_t handle);
void *pool_remove(pool *pl, uint64_t handle);   /* returns item or NULL */

void details_clear(cdin_edit_result *out);
void details_limit(cdin_edit_result *out, uint32_t limit_id);

/* ---- notif/wake_list.c --------------------------------------------------------- */
void wake_run(wake_list *wl);
void wake_add(wake_list *wl, void (*fn)(void *), void *ctx);   /* deduplicated */

/* ---- doc/doc.c ----------------------------------------------------------------- */
/* Resolves a handle; handles stopped kernels. Takes the lock, so callers are inside
 * a locked region already. */
cdin_status doc_lookup(cdin_kernel *k, cdin_doc d, doc **out);
/* Same, but rejects stopped kernels: for MUTating calls (CONC-020). Reads and
 * close keep working after shutdown. */
cdin_status doc_lookup_mut(cdin_kernel *k, cdin_doc d, doc **out);
/* source -> text block + version; takes and releases the kernel lock (api/text.c). */
cdin_status text_qbegin(cdin_kernel *k, cdin_source src, text **t, cdin_version *v);
/* Same resolution without the lock; for api/search (it manages its own window). */
cdin_status src_lookup(cdin_kernel *k, cdin_source src, text **t, cdin_version *v);

cdin_status doc_create_locked(cdin_kernel *k, const uint8_t *bytes, size_t len, cdin_doc *out);
cdin_status doc_close_locked(cdin_kernel *k, doc *d, wake_list *wl);
cdin_status doc_reload_locked(cdin_kernel *k, doc *d, const cdin_reload_request *req,
                              cdin_edit_result *out, wake_list *wl);
cdin_status doc_mark_clean_locked(cdin_kernel *k, doc *d, wake_list *wl);
cdin_status doc_history_info_locked(cdin_kernel *k, doc *d, uint64_t *undo_units,
                                    uint64_t *redo_units, uint64_t *evicted_total,
                                    uint64_t *memory_bytes);
cdin_status snapshot_acquire_locked(cdin_kernel *k, doc *d, cdin_version *out_version,
                                    cdin_snapshot *out);
cdin_status snapshot_release_locked(cdin_kernel *k, cdin_snapshot sh);

/* Frees units, text and the live transaction of a document. */
void doc_free_contents(cdin_kernel *k, doc *d);
/* Emits the final `closed` event, detaches subscriptions, removes and frees the document. */
void doc_close_internal(cdin_kernel *k, doc *d, wake_list *wl);

/* ---- doc/edit.c ---------------------------------------------------------------- */
cdin_status out_check(const cdin_edit_result *out);
int doc_is_clean(const doc *d);

cdin_status doc_replace_locked(cdin_kernel *k, cdin_doc dh, const cdin_replace_request *req,
                               cdin_edit_result *out, wake_list *wl);
cdin_status doc_batch_locked(cdin_kernel *k, cdin_doc dh, const cdin_batch_request *req,
                             cdin_edit_result *out, wake_list *wl);
cdin_status history_locked(cdin_kernel *k, cdin_doc dh, const cdin_history_request *req,
                           cdin_edit_result *out, int undo, wake_list *wl);

/* Builds the record of replacing [start,end) of t with bytes (copies; NO_MEMORY changes nothing). */
cdin_status edit_rec_make(cdin_kernel *k, const text *t, uint64_t start, uint64_t end,
                          const uint8_t *bytes, size_t len, edit_rec *r);
void unit_free(cdin_kernel *k, unit *u);
void edits_free(cdin_kernel *k, edit_rec *e, size_t n);
size_t edit_bytes(const edit_rec *e);
int doc_is_clean(const doc *d);
cdin_status units_reserve(cdin_kernel *k, doc *d, size_t want);
void units_drop_redo(cdin_kernel *k, doc *d);
void evict_oldest(cdin_kernel *k, doc *d);

/* Validation of a request against a tentative text (transactions). */
cdin_status txn_replace_validate(cdin_kernel *k, const text *t, const cdin_replace_request *req,
                                 cdin_edit_result *out);

typedef struct blobs {
    const uint8_t *before; size_t before_len;
    const uint8_t *after;  size_t after_len;
} blobs;

cdin_status blobs_check(cdin_kernel *k, const blobs *b, cdin_edit_result *out);

typedef enum commit_kind { CK_EDIT, CK_BATCH, CK_TXN } commit_kind;

/* A fully prepared commit: every allocation that could fail has been done. */
typedef struct commit_prep {
    int coalesce;            /* merge into the open unit (the last one) */
    uint8_t *blob_before;    size_t blob_before_len;   /* new unit only */
    uint8_t *blob_after;     size_t blob_after_len;
    size_t delta_bytes;      /* accounted bytes this commit adds */
} commit_prep;

/* `edits` is an array of exactly n records allocated with k_alloc(n*sizeof(edit_rec));
 * on success the unit (or the merge) takes ownership of the array and its buffers. */
cdin_status commit_prepare(cdin_kernel *k, doc *d, commit_kind kind, const edit_rec *edits, size_t n,
                           uint64_t key, const blobs *b, commit_prep *cp);
void commit_abort(cdin_kernel *k, commit_prep *cp);
/* Infallible. The text change is already applied to d->txt. */
void commit_finish(cdin_kernel *k, doc *d, commit_kind kind, commit_prep *cp, edit_rec *edits,
                   size_t n, uint64_t key, wake_list *wl, cdin_edit_result *out);

cdin_status accept_check(cdin_kernel *k, doc *d, cdin_version expected, cdin_edit_result *out);
void emit_doc_event(cdin_kernel *k, doc *d, cdin_notif_kind kind, cdin_version from, cdin_version to,
                    const edit_rec *edits, size_t n, int inverse, wake_list *wl);
void emit_simple(cdin_kernel *k, doc *d, cdin_notif_kind kind, uint64_t evicted, int is_clean,
                 wake_list *wl);
void txn_free(cdin_kernel *k, txn_state *tx);          /* releases work text and edits, frees tx */
/* Marks every subscription of d as closed (events already queued stay readable). */
void subs_detach_doc(cdin_kernel *k, doc *d);

/* ---- txn/txn.c ----------------------------------------------------------------- */
/* Resolves a handle for the api layer ({SHUTDOWN | INVALID_HANDLE | INTERNAL}). */
cdin_status txn_resolve(cdin_kernel *k, cdin_txn th, txn_state **tx, doc **d);
cdin_status txn_begin_locked(cdin_kernel *k, doc *d, const cdin_txn_begin_request *req,
                             cdin_txn *out);
cdin_status txn_replace_locked(cdin_kernel *k, txn_state *tx, doc *d,
                               const cdin_replace_request *req, cdin_edit_result *out);
cdin_status txn_commit_locked(cdin_kernel *k, txn_state *tx, doc *d,
                              const cdin_txn_commit_request *req, cdin_edit_result *out,
                              wake_list *wl);
cdin_status txn_abort_locked(cdin_kernel *k, txn_state *tx, doc *d);
cdin_status txn_len_locked(cdin_kernel *k, txn_state *tx, doc *d, uint64_t *out);
cdin_status txn_read_locked(cdin_kernel *k, txn_state *tx, doc *d, uint64_t start, uint64_t end,
                            uint8_t *buf, size_t bufsz, size_t *out_len);

/* ---- notif/notif.c ------------------------------------------------------------- */
cdin_status notif_subscribe_locked(cdin_kernel *k, cdin_doc dh, const cdin_subscribe_request *req,
                                   cdin_version *out_version, cdin_notif *out);
cdin_status notif_unsubscribe_locked(cdin_kernel *k, cdin_notif nh);
cdin_status notif_drain_locked(cdin_kernel *k, cdin_notif nh, cdin_notif_event *events,
                               size_t max, size_t *out_count);

/* ---- core/block.c --------------------------------------------------------------- */
text *text_new(cdin_kernel *k, size_t cap);
void text_release(cdin_kernel *k, text *t);
/* Makes *slot uniquely owned with capacity >= need. Fails with NO_MEMORY, changing nothing. */
cdin_status text_reserve(cdin_kernel *k, text **slot, size_t need);
/* In-place splice on a uniquely owned text with enough capacity. */
void text_splice(cdin_kernel *k, text *t, uint64_t start, uint64_t removed_len, const uint8_t *ins, size_t ins_len);
/* Called with the kernel lock held; allocates/publishes a build-in-progress marker. */
int text_search_index_begin(cdin_kernel *k, text *t);
/* Called without the kernel lock while the caller pins t. Publishes index + UTF-8 metadata. */
void text_search_index_build(text *t);
cdin_status text_pos_check(const text *t, uint64_t p);      /* OK / OUT_OF_RANGE / INVALID_POSITION */
cdin_status text_build_lines(cdin_kernel *k, text *t);

/* ---- util/utf8.c ---------------------------------------------------------------- */
/* Length (1..4) of the well-formed sequence at s, or 0 if ill-formed. cp may be NULL. */
size_t utf8_decode(const uint8_t *s, size_t n, uint32_t *cp);
/* True if byte p lies strictly inside a well-formed multibyte sequence; reports its start/len. */
int utf8_inside(const uint8_t *d, size_t len, size_t p, size_t *seq_start, size_t *seq_len);
/* Class of byte i: 1 = part of a well-formed sequence, 0 = raw. */
int utf8_byte_valid(const uint8_t *d, size_t len, size_t i);
int utf8_all_valid(const uint8_t *d, size_t len);
/* Boundary of the code-point unit (valid sequence or one raw byte) containing byte p. */
size_t unit_start(const uint8_t *d, size_t len, size_t p);
size_t unit_end(const uint8_t *d, size_t len, size_t p);   /* p must be a unit start */

/* ---- util/mem memory-checked walks live in text/ ---------------------------------- */
/* text/lines.c --------------------------------------------------------------- */
/* Number of terminators in bytes[0..len). */
size_t line_term_count(const uint8_t *s, size_t len);
/* Fills starts with one entry per line (terminators + 1); caller sized it. */
void line_fill_starts(const uint8_t *s, size_t len, uint64_t *starts);
/* Binary search: line number of a byte offset and the offset inside the line. */
void line_of(const uint64_t *starts, size_t nlines, uint64_t offset, uint64_t *line, uint64_t *col);
/* Terminator census (TEXT-022). */
void eol_scan(const uint8_t *s, size_t len, uint64_t *lf, uint64_t *crlf, uint64_t *cr,
              int *unterminated);

/* text/positions.c ------------------------------------------------------------- */
/* Code-point index of the unit at or before byte offset; CDIN_CONV_TOUCHED_RAW in flags. */
cdin_status cp_index_at(const uint8_t *d, size_t len, uint64_t offset, uint64_t *out, uint32_t *flags);
/* Byte offset of the index-th unit; flags as above. */
cdin_status cp_offset_of(const uint8_t *d, size_t len, uint64_t index, uint64_t *out, uint32_t *flags);

/* text/graphemes.c ------------------------------------------------------------- */
/* End of the cluster starting at/before offset (POS-030/033). */
size_t gc_next_boundary(const uint8_t *d, size_t len, uint64_t offset);
size_t gc_prev_boundary(const uint8_t *d, size_t len, uint64_t offset);

/* ---- search/literal.c and search/fold.c ---------------------------------------- */
uint32_t fold_simple(uint32_t cp);
/* The scan itself; the caller has validated everything, pinned the block and holds
 * a reference. Pure except for the atomic cancel load. Writes the result fields. */
cdin_status literal_run(const text *t, const cdin_search_request *r, const uint32_t *pat,
                        size_t mcp, uint64_t step_limit, cdin_search_result *out);
/* Pattern -> folded code-point array for the CASE_INSENSITIVE/ANY_NEWLINE modes. */
cdin_status search_prep_pattern(cdin_kernel *k, const cdin_search_request *r, uint32_t **out,
                                size_t *m);
int search_pattern_valid(const cdin_search_request *r);
/* Fills result->details for the pre-scan failures (stale version, range, position). */
void search_fail_details(cdin_search_result *out, cdin_status st, uint64_t a, uint64_t b);

#endif
