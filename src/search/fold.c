/* Simple case folding for one code point (SEARCH-020).
 *
 * Driven by the pinned generated table; ASCII is the fast path because it
 * covers the overwhelming majority of patterns. Full Unicode folding with
 * normalisation and language profiles is not in this release (docs/limits-and-status.md). */
#include "internal.h"

uint32_t fold_simple(uint32_t cp)
{
    size_t lo = 0, hi = cdin_fold_table_len;
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cdin_fold_table[mid].from == cp) return cdin_fold_table[mid].to;
        if (cdin_fold_table[mid].from < cp) lo = mid + 1; else hi = mid;
    }
    return cp;
}
