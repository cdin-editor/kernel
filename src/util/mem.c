/* Allocator access and checked size arithmetic (ABI-017).
 *
 * The only module in src/util that touches the injected allocator, so a leak or
 * a double free is always attributable to one file. Checked add/mul replace the
 * ad-hoc SIZE_MAX guards: every caller hands the raw sizes in, and the failure
 * decision lives here. */
#include "internal.h"

void *k_alloc(cdin_kernel *k, size_t n)
{
    if (n == 0) return NULL;
    return k->al.alloc(k->al.ctx, n);
}

void *k_realloc(cdin_kernel *k, void *p, size_t old_n, size_t new_n)
{
    if (new_n == 0) return NULL;
    if (!p) return k->al.alloc(k->al.ctx, new_n);
    return k->al.realloc(k->al.ctx, p, old_n, new_n);
}

void k_free(cdin_kernel *k, void *p, size_t n)
{
    if (p) k->al.free(k->al.ctx, p, n);
}

uint8_t *k_dup(cdin_kernel *k, const uint8_t *src, size_t len)
{
    uint8_t *p;
    if (len == 0) return NULL;
    p = k_alloc(k, len);
    if (p) memcpy(p, src, len);
    return p;
}

int k_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return 0;
    *out = a + b;
    return 1;
}

int k_mul(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}
