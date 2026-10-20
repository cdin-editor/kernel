/* Strict allocator for fuzzing and benchmarking (KERNEL-009).
 *
 * Every allocation gets a header with sentinels on both ends and is kept on a
 * live list. free() validates the pointer, the exact size and both sentinels,
 * and poisons the data. strict_check() reports leaks at the end of a run.
 * Use-after-free is caught by the poisoned bytes; the real allocator or ASan
 * catches the rest. Not thread-safe: fuzz targets run single-threaded.
 */
#ifndef CDIN_SUPPORT_STRICT_ALLOC_H
#define CDIN_SUPPORT_STRICT_ALLOC_H

#include <cdin/kernel.h>

/* The full definition is intentional: benchmark and fuzz targets create
 * `strict_alloc` instances directly on the stack. */
typedef struct strict_alloc {
    cdin_allocator vtable;
    struct sa_node *head;
    unsigned long ops;      /* alloc/realloc attempts */
    long fail_at;           /* 0 = never fails */
    int fail_mode;
    int broken;
    size_t live;            /* live blocks */
    size_t peak;            /* max live blocks */
} strict_alloc;


void strict_init(strict_alloc *sa);
/* The vtable for cdin_kernel_config.allocator. */
cdin_allocator strict_vtable(strict_alloc *sa);
/* Fail the nth allocation op (1-based). If mode != 0, also count frees so the
 * failure point is reachable in free-heavy programs. */
void strict_fail_nth(strict_alloc *sa, unsigned n, int mode);
/* 0 if everything allocated was freed and no violation was seen, else nonzero. */
int strict_check(strict_alloc *sa, const char *name);

#endif
