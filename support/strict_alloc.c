#include "strict_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SA_MAGIC UINT64_C(0x535452494354414C) /* "STRICTAL" */

typedef struct sa_node {
    uint64_t magic;
    size_t size;            /* exact user size */
    struct sa_node *prev;
    struct sa_node *next;
    size_t _pad;            /* 48-byte header: data stays 8-aligned */
} sa_node;

/* The only complete definition of strict_alloc lives in the header: the
 * callers instantiate it on the stack. Keeping this copy would drift. */

static void sa_broken(strict_alloc *sa, const char *what)
{
    if (!sa->broken) {
        fprintf(stderr, "strict_alloc: %s\n", what);
        sa->broken = 1;
    }
}

static sa_node *node_of(void *p) { return (sa_node *)((char *)p - sizeof(sa_node)); }

/* Two-byte guard derived from the size, written behind the user data. */
static void canary_put(sa_node *n)
{
    unsigned char *t = (unsigned char *)n + sizeof(*n) + n->size;
    t[0] = (unsigned char)(n->size + 0xA5);
    t[1] = (unsigned char)((n->size ^ 0x5A) | 0x01);
}

static int canary_ok(sa_node *n)
{
    const unsigned char *t = (const unsigned char *)n + sizeof(*n) + n->size;
    return t[0] == (unsigned char)(n->size + 0xA5) &&
           t[1] == (unsigned char)((n->size ^ 0x5A) | 0x01);
}

static int sa_verify(strict_alloc *sa, sa_node *n)
{
    if (n->magic != SA_MAGIC) { sa_broken(sa, "bad magic (double free or wild pointer)"); return 0; }
    if (!canary_ok(n)) { sa_broken(sa, "canary clobbered (buffer overrun)"); return 0; }
    return 1;
}

static void sa_link(strict_alloc *sa, sa_node *n)
{
    n->prev = NULL;
    n->next = sa->head;
    if (sa->head) sa->head->prev = n;
    sa->head = n;
    sa->live++;
    if (sa->live > sa->peak) sa->peak = sa->live;
}

static void sa_unlink(strict_alloc *sa, sa_node *n)
{
    if (n->prev) n->prev->next = n->next; else sa->head = n->next;
    if (n->next) n->next->prev = n->prev;
    sa->live--;
}

static void *sa_alloc(void *ctx, size_t n)
{
    strict_alloc *sa = (strict_alloc *)ctx;
    sa_node *n2;
    unsigned char *data;
    if (n == 0) { sa_broken(sa, "zero-size allocation"); return NULL; }
    sa->ops++;
    if (sa->fail_at > 0 && (unsigned long)sa->fail_at == sa->ops - 1u) return NULL;
    n2 = (sa_node *)malloc(sizeof(sa_node) + n + 2);
    if (!n2) { sa_broken(sa, "out of host memory"); return NULL; }
    n2->magic = SA_MAGIC;
    n2->size = n;
    data = (unsigned char *)n2 + sizeof(sa_node);
    memset(data, 0xCD, n);                     /* "uninitialised" poison */
    canary_put(n2);
    sa_link(sa, n2);
    return data;
}

static void *sa_realloc(void *ctx, void *p, size_t old_n, size_t new_n)
{
    strict_alloc *sa = (strict_alloc *)ctx;
    sa_node *n;
    if (!p) { sa_broken(sa, "realloc(NULL)"); return NULL; }
    if (new_n == 0) { sa_broken(sa, "realloc to zero"); return NULL; }
    n = node_of(p);
    if (!sa_verify(sa, n)) return NULL;
    if (n->size != old_n) { sa_broken(sa, "realloc size mismatch"); return NULL; }
    sa->ops++;
    if (sa->fail_at > 0 && (unsigned long)sa->fail_at == sa->ops - 1u) return NULL;
    {
        /* Realloc in place would lose the sentinels' positions only via the
         * move; rebuilding keeps canaries correct. Larger-enterprise allocators
         * do the same. */
        void *fresh = sa->vtable.alloc(ctx, new_n);
        if (!fresh) return NULL;
        memcpy(fresh, p, n->size < new_n ? n->size : new_n);
        sa_unlink(sa, n);                 /* freed here: also leave the list */
        n->magic = 0;                     /* second free will be caught */
        free(node_of(p));
        return fresh;
    }
}

static void sa_free(void *ctx, void *p, size_t n)
{
    strict_alloc *sa = (strict_alloc *)ctx;
    sa_node *n2;
    if (!p) { sa_broken(sa, "free(NULL)"); return; }
    n2 = node_of(p);
    if (!sa_verify(sa, n2)) return;
    if (n2->size != n) { sa_broken(sa, "free size mismatch"); return; }
    sa_unlink(sa, n2);
    n2->magic = 0;                              /* second free will be caught */
    memset(p, 0xDD, n2->size);                  /* use-after-free poison */
    free(n2);
}

void strict_init(strict_alloc *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->vtable.alloc = sa_alloc;
    sa->vtable.realloc = sa_realloc;
    sa->vtable.free = sa_free;
    sa->vtable.ctx = sa;                 /* was never set: the first kernel
                                          * allocation dereferenced NULL */
    sa->vtable.struct_size = (uint32_t)sizeof(cdin_allocator);
}

cdin_allocator strict_vtable(strict_alloc *sa) { return sa->vtable; }

void strict_fail_nth(strict_alloc *sa, unsigned n, int mode)
{
    sa->fail_at = (long)n;
    sa->fail_mode = mode;
}

int strict_check(strict_alloc *sa, const char *name)
{
    if (sa->broken) { fprintf(stderr, "strict: %s: allocator violation\n", name); return 1; }
    if (sa->live != 0) {
        fprintf(stderr, "strict: %s: %zu block(s) leaked\n", name, sa->live);
        return 1;
    }
    return 0;
}
