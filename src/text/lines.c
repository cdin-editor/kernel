/* Line arithmetic, pure on (bytes, len) (TEXT-020..023).
 *
 * No allocator, no block, no lock: the block layer (core/block.c) sizes and
 * owns the line-start array and calls in here to count and fill it, and the
 * api layer calls in for the per-query thermodynamic pieces (binary search,
 * eol census). A terminator is LF, CRLF (one) or a lone CR. */
#include "internal.h"

size_t line_term_count(const uint8_t *s, size_t len)
{
    size_t count = 0, i;
    if (!s) return 0;
    for (i = 0; i < len;) {
        if (s[i] == 0x0A) { count++; i++; }
        else if (s[i] == 0x0D) {
            count++;
            i += (i + 1 < len && s[i + 1] == 0x0A) ? 2 : 1;
        } else {
            i++;
        }
    }
    return count;
}

void line_fill_starts(const uint8_t *s, size_t len, uint64_t *starts)
{
    size_t i, n = 1;
    starts[0] = 0;
    if (!s) return;
    for (i = 0; i < len;) {
        if (s[i] == 0x0A) { starts[n++] = (uint64_t)i + 1; i++; }
        else if (s[i] == 0x0D) {
            if (i + 1 < len && s[i + 1] == 0x0A) i++;
            starts[n++] = (uint64_t)i + 1;
            i++;
        } else {
            i++;
        }
    }
}

void line_of(const uint64_t *starts, size_t nlines, uint64_t offset, uint64_t *line, uint64_t *col)
{
    size_t lo = 0, hi = nlines;                 /* largest i with starts[i] <= offset */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (starts[mid] <= offset) lo = mid; else hi = mid;
    }
    *line = lo;
    *col = offset - starts[lo];
}

void eol_scan(const uint8_t *s, size_t len, uint64_t *lf, uint64_t *crlf, uint64_t *cr,
              int *unterminated)
{
    size_t i;
    if (lf) *lf = 0;
    if (crlf) *crlf = 0;
    if (cr) *cr = 0;
    if (unterminated) *unterminated = len > 0 && s[len - 1] != 0x0A && s[len - 1] != 0x0D;
    if (!s) return;
    for (i = 0; i < len; i++) {
        if (s[i] == 0x0A) { if (lf) (*lf)++; }
        else if (s[i] == 0x0D) {
            if (i + 1 < len && s[i + 1] == 0x0A) { if (crlf) (*crlf)++; i++; }
            else if (cr) (*cr)++;
        }
    }
}
