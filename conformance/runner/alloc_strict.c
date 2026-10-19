#include "alloc_strict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_HDR 48u
#define BLOCK_MAGIC ((size_t)0xC0D1AB1Eu)

typedef struct alloc_block {
    size_t size;               /* bytes the caller asked for; the contract value */
    struct alloc_ctx *owner;
    size_t magic;
    struct alloc_block *next;
    const char *op;            /* label of the operation that allocated it */
} alloc_block;

static const char *g_op = "?";

/* Test-only harness lock. Acquire order is never nested: the kernel never calls
 * back into the allocator. */
static void gate_in(alloc_ctx *ctx)
{
    while (atomic_exchange_explicit(&ctx->gate, 1u, memory_order_acquire)) { }
}

static void gate_out(alloc_ctx *ctx)
{
    atomic_store_explicit(&ctx->gate, 0u, memory_order_release);
}

/* `message` is already formatted: no call site forwards a runtime format
 * string, which clang's -Wformat-nonliteral rejects under -Wformat=2. */
static void report(alloc_ctx *ctx, const char *message)
{
    snprintf(ctx->last_violation, sizeof(ctx->last_violation), "%s", message);
    ctx->violations++;
    fprintf(stderr, "allocator %s: %s\n", ctx->name, ctx->last_violation);
}

static alloc_block *block_of(alloc_ctx *ctx, void *p)
{
    alloc_block *b = (alloc_block *)((char *)p - BLOCK_HDR);
    if (b->magic != BLOCK_MAGIC || b->owner != ctx) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: block %p is not owned by this allocator", g_op, p);
        report(ctx, msg);
        return NULL;
    }
    return b;
}

static void list_unlink(alloc_ctx *ctx, alloc_block *b)
{
    alloc_block **link = &ctx->head;
    while (*link) {
        if (*link == b) { *link = b->next; return; }
        link = &(*link)->next;
    }
}

static void *do_alloc(alloc_ctx *ctx, size_t n)
{
    if (ctx->mode & ALLOC_COUNTING) {
        if (ctx->fail_after != UINT64_MAX && ctx->count >= ctx->fail_after) return NULL;
    }
    alloc_block *b = (alloc_block *)malloc(n + BLOCK_HDR);
    if (!b) return NULL;
    ctx->count++;
    ctx->total_allocs++;
    b->size = n;
    b->owner = ctx;
    b->magic = BLOCK_MAGIC;
    b->op = g_op;
    b->next = ctx->head;
    ctx->head = b;
    ctx->live_blocks++;
    void *p = (char *)b + BLOCK_HDR;
    if (ctx->mode & ALLOC_POISON) memset(p, ALLOC_POISON_BYTE, n);
    return p;
}

static void *do_realloc(void *vctx, void *p, size_t old_n, size_t new_n);
static void do_free(void *vctx, void *p, size_t n);

static void *a_alloc(void *vctx, size_t n)
{
    alloc_ctx *ctx = (alloc_ctx *)vctx;
    void *p;
    gate_in(ctx);
    p = do_alloc(ctx, n);
    gate_out(ctx);
    return p;
}

static void *a_realloc(void *vctx, void *p, size_t old_n, size_t new_n)
{
    alloc_ctx *ctx = (alloc_ctx *)vctx;
    void *out;
    gate_in(ctx);
    out = do_realloc(ctx, p, old_n, new_n);
    gate_out(ctx);
    return out;
}

static void a_free(void *vctx, void *p, size_t n)
{
    alloc_ctx *ctx = (alloc_ctx *)vctx;
    if (!p) return;
    gate_in(ctx);
    do_free(ctx, p, n);
    gate_out(ctx);
}

static void *do_realloc(void *vctx, void *p, size_t old_n, size_t new_n)
{
    alloc_ctx *ctx = (alloc_ctx *)vctx;
    if (!p) return new_n ? do_alloc(ctx, new_n) : NULL; /* realloc(NULL, 0, n) allocates */

    alloc_block *b = block_of(ctx, p);
    if (!b) return NULL;
    if ((ctx->mode & ALLOC_STRICT) && old_n != b->size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: realloc reports old size %llu, allocated %llu",
                 g_op, (unsigned long long)old_n, (unsigned long long)b->size);
        report(ctx, msg);
    }
    if (new_n == 0) {
        b->magic = 0;
        list_unlink(ctx, b);
        ctx->live_blocks--;
        free(b);
        return NULL;
    }
    if ((ctx->mode & ALLOC_COUNTING) &&
        ctx->fail_after != UINT64_MAX && ctx->count >= ctx->fail_after)
        return NULL;

    /* Move or grow in place. ALWAYS_MOVE never keeps the old block, so a
     * memmove that still reads from the old pointer reads poisoned bytes. */
    size_t keep = b->size < new_n ? b->size : new_n;
    const char *op = b->op;
    int move = (ctx->mode & ALLOC_MOVE) != 0;
    alloc_block *nb;

    /* Unlink first: after this point the old block must never be named again. */
    list_unlink(ctx, b);
    if (move) {
        void *fresh = malloc(new_n + BLOCK_HDR);
        if (!fresh) { b->next = ctx->head; ctx->head = b; return NULL; }
        memcpy((char *)fresh + BLOCK_HDR, p, keep);
        nb = (alloc_block *)fresh;
        ctx->live_blocks++;  /* the new block replaces the old one */
        ctx->count++;
        ctx->total_allocs++;
        memset(p, ALLOC_MOVED_BYTE, b->size); /* stale reads now return 0xDD */
        free(b);
        ctx->live_blocks--; /* the old block is gone, the new one is counted */
    } else {
        void *fresh = realloc(b, new_n + BLOCK_HDR);
        if (!fresh) { b->next = ctx->head; ctx->head = b; return NULL; }
        nb = (alloc_block *)fresh; /* same allocation: live_blocks is unchanged */
        ctx->count++;
        ctx->total_allocs++;
    }
    nb->size = new_n;
    nb->op = op;
    nb->owner = ctx;
    nb->magic = BLOCK_MAGIC;
    nb->next = ctx->head;
    ctx->head = nb;
    return (char *)nb + BLOCK_HDR;
}

static void do_free(void *vctx, void *p, size_t n)
{
    alloc_ctx *ctx = (alloc_ctx *)vctx;
    alloc_block *b = block_of(ctx, p);
    if (!b) return;
    if ((ctx->mode & ALLOC_STRICT) && n != b->size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: free reports size %llu, allocated %llu",
                 g_op, (unsigned long long)n, (unsigned long long)b->size);
        report(ctx, msg);
    }
    b->magic = 0;
    list_unlink(ctx, b);
    ctx->live_blocks--;
    free(b);
}

void alloc_init(alloc_ctx *ctx, const char *name, unsigned mode)
{
    memset(ctx, 0, sizeof(*ctx));
    atomic_init(&ctx->gate, 0u);
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);
    ctx->mode = mode;
    ctx->fail_after = UINT64_MAX;
    ctx->vtable.struct_size = sizeof(cdin_allocator);
    ctx->vtable.alloc = a_alloc;
    ctx->vtable.realloc = a_realloc;
    ctx->vtable.free = a_free;
    ctx->vtable.ctx = ctx;
}

cdin_allocator alloc_vtable(alloc_ctx *ctx)
{
    return ctx->vtable;
}

void alloc_set_op(alloc_ctx *ctx, const char *op)
{
    (void)ctx;
    g_op = op;
}

uint64_t alloc_report_live(alloc_ctx *ctx, const char *when)
{
    if (ctx->head == NULL) {
        if (ctx->live_blocks != 0)
            fprintf(stderr, "allocator %s: %s, list lost %llu block(s) (harness bug)\n",
                    ctx->name, when, (unsigned long long)ctx->live_blocks);
        return 0;
    }
    /* One line per (op, size) group so the inventory names the allocation site. */
    for (alloc_block *a = ctx->head; a; a = a->next) {
        int printed = 0;
        for (alloc_block *b = a->next; b; b = b->next)
            if (b->op == a->op && b->size == a->size) { printed = 1; break; }
        if (printed) continue;
        uint64_t n = 0;
        for (alloc_block *b = a; b; b = b->next)
            if (b->op == a->op && b->size == a->size) n++;
        fprintf(stderr, "allocator %s: %s, %llu block(s) of %llu byte(s) allocated by '%s'\n",
                ctx->name, when, (unsigned long long)n, (unsigned long long)a->size, a->op);
        ctx->violations += n;
    }
    return ctx->live_blocks;
}

uint64_t alloc_violations(const alloc_ctx *ctx)
{
    return ctx->violations;
}
