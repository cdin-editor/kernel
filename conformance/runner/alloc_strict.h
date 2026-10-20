/*
 * Block-tracked allocator for conformance vectors and C tests.
 *
 * Every block carries a header with its true size and the label of the
 * operation that allocated it, so the harness can prove three things the spec
 * requires (ABI-017, ERR-005, TEST-012):
 *   - the kernel passes the true block size to free() and realloc();
 *   - nothing is still live when the kernel is destroyed;
 *   - nothing is read after a block was released (mode ALWAYS_MOVE poisons the
 *     old block so a stale pointer produces a deterministic wrong answer).
 *
 * Modes: COUNTING (optionally fails the Nth allocation), STRICT, POISON (fills
 * new blocks with 0xAA), ALWAYS_MOVE. They combine; the runner and the tests use
 * different combinations.
 */
#ifndef ALLOC_STRICT_H
#define ALLOC_STRICT_H

#include <cdin/kernel.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define ALLOC_POISON_BYTE 0xAA
#define ALLOC_MOVED_BYTE 0xDD

typedef struct alloc_ctx alloc_ctx;

enum {
    ALLOC_COUNTING = 1u << 0,  /* fail the fail_after'th allocation when fail_after < UINT64_MAX */
    ALLOC_STRICT = 1u << 1,    /* check sizes, report double free and leaks */
    ALLOC_POISON = 1u << 2,    /* fill fresh bytes with 0xAA */
    ALLOC_MOVE = 1u << 3       /* always relocate, then poison the released bytes with 0xDD */
};

struct alloc_ctx {
    char name[32];
    unsigned mode;
    uint64_t count;        /* successful allocations so far */
    uint64_t fail_after;   /* allocations allowed before failing; UINT64_MAX = never */
    uint64_t live_blocks;
    uint64_t total_allocs;
    uint64_t violations;
    char last_violation[160];
    cdin_allocator vtable;
    struct alloc_block *head;
    /* Snapshot storage may be freed outside the kernel lock, so the harness
     * allocation list can be accessed by multiple threads at once. */
    atomic_uint gate;
};

/* Creates a context. The returned vtable is what goes into cdin_kernel_config. */
void alloc_init(alloc_ctx *ctx, const char *name, unsigned mode);
cdin_allocator alloc_vtable(alloc_ctx *ctx);

/* Labels every subsequent allocation; the label is a static string owned by the
 * caller (the runner passes the operation name). */
void alloc_set_op(alloc_ctx *ctx, const char *op);

/* Prints one line per still-live block grouped by (op, size). Returns the
 * number of live blocks. Used at the end of a vector or after destroy. */
uint64_t alloc_report_live(alloc_ctx *ctx, const char *when);

/* Number of leaks found so far; 0 means destroy was clean. */
uint64_t alloc_violations(const alloc_ctx *ctx);

#endif
