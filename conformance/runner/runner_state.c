#include "runner_state.h"
#include "alloc_strict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

allocator_entry g_allocators[MAX_ALLOCATORS];
int g_allocator_count = 0;
handle_entry g_handles[MAX_HANDLES];
cdin_kernel *g_kernel = NULL;
cdin_status g_last_status;
vec_out g_out;
char g_last_handle[HANDLE_NAME_MAX] = "";
uint64_t g_kernel_seq = 0;
uint64_t g_search_ranges[16];
int g_alloc_violations = 0;

static vec_out g_out_snapshot;
/* Handle names restart at k1/ks1 for every vector (FORMAT.md). */
static int g_seq_k, g_seq_ks;

void state_reset(void)
{
    handles_destroy_all();
    g_kernel = NULL;
    g_last_status = CDIN_OK;
    g_last_handle[0] = '\0';
    g_kernel_seq = 0;
    g_seq_k = 0;
    g_seq_ks = 0;
    memset(&g_out, 0, sizeof(g_out));
    memset(&g_out_snapshot, 0, sizeof(g_out_snapshot));
    for (int i = 0; i < g_allocator_count; i++) {
        alloc_ctx *ctx = g_allocators[i].ctx;
        ctx->count = 0;
        ctx->fail_after = UINT64_MAX;
    }
}

/* Leaks are only meaningful once every kernel using the allocator is gone, so
 * the report runs at the end of a vector, not after each destroy. */
void allocator_report(void)
{
    for (int i = 0; i < g_allocator_count; i++) {
        alloc_ctx *ctx = g_allocators[i].ctx;
        if (ctx->live_blocks == 0) continue;
        alloc_report_live(ctx, "leak at end of vector");
    }
}

void allocator_gather_violations(void)
{
    for (int i = 0; i < g_allocator_count; i++)
        g_alloc_violations += (int)alloc_violations(g_allocators[i].ctx);
}

alloc_ctx *allocator_get(const char *name)
{
    for (int i = 0; i < g_allocator_count; i++)
        if (strcmp(g_allocators[i].name, name) == 0) return g_allocators[i].ctx;
    if (g_allocator_count >= MAX_ALLOCATORS) return NULL;
    allocator_entry *e = &g_allocators[g_allocator_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    static alloc_ctx contexts[MAX_ALLOCATORS];
    alloc_ctx *ctx = &contexts[g_allocator_count - 1];
    /* counting: failure injection. strict: size and leak checking. */
    alloc_init(ctx, name, ALLOC_COUNTING | ALLOC_STRICT);
    e->ctx = ctx;
    return ctx;
}

int handle_new_named(handle_kind kind, cdin_kernel *k, uint64_t value, const char *name)
{
    char generated[HANDLE_NAME_MAX];
    if (!name) {
        if (kind == HK_NOTIF || kind == HK_SNAP)
            snprintf(generated, sizeof(generated), "ks%d", ++g_seq_ks);
        else
            snprintf(generated, sizeof(generated), "k%d", ++g_seq_k);
        name = generated;
    }
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (g_handles[i].valid) continue;
        size_t n = strlen(name);
        if (n >= HANDLE_NAME_MAX) n = HANDLE_NAME_MAX - 1;
        memcpy(g_handles[i].name, name, n);
        g_handles[i].name[n] = '\0';
        g_handles[i].kind = kind;
        g_handles[i].k = k;
        g_handles[i].value = value;
        g_handles[i].valid = 1;
        if (n) memcpy(g_last_handle, name, n);
        g_last_handle[n] = '\0';
        return 1;
    }
    return 0;
}

int handle_new(handle_kind kind, cdin_kernel *k, uint64_t value)
{
    return handle_new_named(kind, k, value, NULL);
}

const handle_entry *handle_get(const char *name, handle_kind kind)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (g_handles[i].valid && g_handles[i].kind == kind && strcmp(g_handles[i].name, name) == 0)
            return &g_handles[i];
    return NULL;
}

void handle_drop(const char *name)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (g_handles[i].valid && strcmp(g_handles[i].name, name) == 0)
            g_handles[i].valid = 0;
}

void handles_destroy_all(void)
{
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (g_handles[i].valid && g_handles[i].kind == HK_KERNEL && g_handles[i].k) {
            cdin_kernel_destroy(g_handles[i].k);
            g_handles[i].valid = 0;
        }
    }
    memset(g_handles, 0, sizeof(g_handles));
}

cdin_status kernel_ensure(uint64_t max_documents, uint64_t max_document_size,
                          uint64_t max_undo_memory, uint64_t max_blob_size)
{
    if (g_kernel) return CDIN_OK;
    alloc_ctx *ctx = allocator_get("counting");
    if (!ctx) return CDIN_INTERNAL;
    cdin_kernel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator = alloc_vtable(ctx);
    cfg.max_documents = max_documents;
    cfg.max_document_size = max_document_size;
    cfg.max_undo_memory = max_undo_memory;
    cfg.max_blob_size = max_blob_size;
    /* notif.basic.resync_on_overflow is authored against a 4-deep ring (the
     * published default is 64): set it explicitly so the collapse to one RESYNC
     * is exercised. */
    cfg.notif_queue_capacity = 4;
    cdin_kernel *k = NULL;
    cdin_status st = cdin_kernel_create(&cfg, &k);
    if (st != CDIN_OK) return st;
    if (!handle_new_named(HK_KERNEL, k, (uint64_t)(uintptr_t)k, "")) {
        cdin_kernel_destroy(k);
        return CDIN_INTERNAL;
    }
    g_kernel = k;
    return CDIN_OK;
}

void out_begin(void)
{
    memcpy(&g_out_snapshot, &g_out, sizeof(g_out));
}

int out_untouched(const char *what, const char *path, int line)
{
    /* ABI 1.0.0: on failure the kernel may fill `details` and nothing else.
     * Mask it in both copies, otherwise every detail write looks like a wild
     * out-parameter write. */
    vec_out cur = g_out, snap = g_out_snapshot;
    memset(&cur.edit.details, 0, sizeof cur.edit.details);
    memset(&snap.edit.details, 0, sizeof snap.edit.details);
    memset(&cur.search.details, 0, sizeof cur.search.details);
    memset(&snap.search.details, 0, sizeof snap.search.details);
    if (memcmp(&cur, &snap, sizeof(cur)) == 0) return 0;
    fprintf(stderr, "%s:%d: out parameters modified on failure (%s)\n", path, line, what);
    return 1;
}
