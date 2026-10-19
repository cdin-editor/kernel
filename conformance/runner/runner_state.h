/* Runner state: typed handle table, injected allocators (plain / counting /
 * strict), and the single output block that `expect` lines read. Every field in
 * vec_out is written by the kernel, so the whole block doubles as the
 * out-parameter sentinel check. */
#ifndef RUNNER_STATE_H
#define RUNNER_STATE_H

#include "cdin/kernel.h"
#include "alloc_strict.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_HANDLES 32
#define MAX_ALLOCATORS 8

typedef enum { HK_KERNEL, HK_DOC, HK_TXN, HK_NOTIF, HK_SNAP } handle_kind;

/* handle=ksN for notifications and snapshots, handle=kN otherwise (FORMAT.md). */
#define HANDLE_NAME_MAX 24

typedef struct {
    char name[HANDLE_NAME_MAX];
    handle_kind kind;
    cdin_kernel *k;
    uint64_t value;
    int valid;
} handle_entry;

typedef struct {
    char name[32];
    alloc_ctx *ctx;
} allocator_entry;

/* Only fields the kernel writes through an out-parameter live here. */
typedef struct {
    uint64_t num_version;      /* op-dependent: doc / search / snapshot / subscribe */
    uint64_t num_count;        /* op-dependent: search / notif / line count */
    uint64_t num_length;
    uint64_t num_offset;
    uint64_t num_codepoint;
    uint64_t num_line;
    uint64_t num_col;
    uint64_t num_new_version;
    uint64_t num_start;
    uint64_t num_removed;
    uint64_t num_inserted;
    uint64_t num_new_length;
    uint64_t num_current_version;
    uint64_t num_from_version;
    uint64_t num_to_version;
    uint64_t num_next;
    uint64_t num_prev;
    uint64_t num_clean;
    uint64_t num_truncated;    /* search: the result array filled up */
    uint64_t num_valid;
    uint64_t num_abi_major, num_abi_minor, num_abi_patch;
    uint64_t num_features;
    uint64_t num_seq;
    uint64_t num_eol_style;
    uint64_t notif_kind;
    uint8_t data[4096];
    size_t data_len;
    cdin_edit_result edit;
    cdin_search_result search;
    cdin_notif_event events[64];
    size_t event_count;
} vec_out;

extern allocator_entry g_allocators[MAX_ALLOCATORS];
extern int g_allocator_count;
extern handle_entry g_handles[MAX_HANDLES];
extern cdin_kernel *g_kernel;
extern cdin_status g_last_status;
extern vec_out g_out;
extern char g_last_handle[HANDLE_NAME_MAX];
extern uint64_t g_kernel_seq;
/* Caller-owned match buffer for cdin_search (ops.c fills `ranges`, expect.c checks). */
extern uint64_t g_search_ranges[16];
extern int g_alloc_violations;

void state_reset(void);

/* Allocators. Returns NULL for an unknown name. */
alloc_ctx *allocator_get(const char *name);
void allocators_reset(void);
/* Leaks at the end of a vector, attributed to the operation that allocated. */
void allocator_report(void);
void allocator_gather_violations(void);

/* Handles. `handle_new` allocates the next auto-generated name. */
int handle_new(handle_kind kind, cdin_kernel *k, uint64_t value);
int handle_new_named(handle_kind kind, cdin_kernel *k, uint64_t value, const char *name);
const handle_entry *handle_get(const char *name, handle_kind kind);
void handle_drop(const char *name);
void handles_destroy_all(void);

cdin_status kernel_ensure(uint64_t max_documents, uint64_t max_document_size,
                          uint64_t max_undo_memory, uint64_t max_blob_size);

/* Out-parameter sentinel: snapshot before the call, compare after a failure. */
void out_begin(void);
int out_untouched(const char *what, const char *path, int line);

#endif
