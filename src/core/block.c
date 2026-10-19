/* Content block (core of the text engine): reference-counted, copy-on-write.
 *
 * A document owns one reference; every live snapshot and every running search
 * owns one more. A mutation writes in place only while refs == 1; a shared
 * block is cloned first (text_reserve), so a reader never sees bytes move.
 * The cached line index lives here because it belongs to the block's content:
 * any in-place splice invalidates it in O(1). */
#include "internal.h"

#define TEXT_HDR offsetof(text, data)

text *text_new(cdin_kernel *k, size_t cap)
{
    text *t;
    size_t total;
    if (cap > SIZE_MAX - TEXT_HDR) return NULL;
    if (!k_add(TEXT_HDR, cap, &total)) return NULL;
    t = k_alloc(k, total);
    if (!t) return NULL;
    t->refs = 1;
    t->len = 0;
    t->cap = cap;
    t->lines = NULL;
    t->nlines = 0;
    t->lines_cap = 0;
    t->lines_valid = 0;
    return t;
}

void text_release(cdin_kernel *k, text *t)
{
    if (!t) return;
    if (--t->refs > 0) return;
    k_free(k, t->lines, t->lines_cap * sizeof(uint64_t));
    k_free(k, t, TEXT_HDR + t->cap);
}

static size_t grow_cap(size_t need, size_t cur)
{
    size_t c = cur + cur / 2;
    if (c < need) c = need;
    if (c < 64) c = 64;
    if (need <= SIZE_MAX / 2 && c > need * 2) c = need * 2;
    return c;
}

cdin_status text_reserve(cdin_kernel *k, text **slot, size_t need)
{
    text *t = *slot;
    if (t->refs > 1) {                         /* shared: clone (copy-on-write) */
        size_t want = need < t->len ? t->len : need;
        size_t ncap = want <= SIZE_MAX / 2 ? want + want / 8 + 64 : want;      /* headroom: the next edit must not realloc */
        text *n = text_new(k, ncap);
        if (!n) return CDIN_NO_MEMORY;
        memcpy(n->data, t->data, t->len);
        n->len = t->len;
        t->refs--;
        *slot = n;
        return CDIN_OK;
    }
    if (t->cap < need) {
        size_t ncap = grow_cap(need, t->cap), need_bytes = 0;
        text *n;
        if (ncap > SIZE_MAX - TEXT_HDR) ncap = need;      /* the plain need always fits below */
        k_add(TEXT_HDR, ncap, &need_bytes);
        n = k_realloc(k, t, TEXT_HDR + t->cap, need_bytes);
        if (!n) {
            /* retry with the exact size before giving up */
            k_add(TEXT_HDR, need, &need_bytes);
            n = k_realloc(k, t, TEXT_HDR + t->cap, need_bytes);
            if (!n) return CDIN_NO_MEMORY;
            ncap = need;
        }
        n->cap = ncap;
        *slot = n;
    }
    return CDIN_OK;
}

void text_splice(text *t, uint64_t start, uint64_t removed_len, const uint8_t *ins, size_t ins_len)
{
    size_t tail_from = (size_t)(start + removed_len);
    size_t tail_len = t->len - tail_from;
    if (tail_len && ins_len != removed_len)
        memmove(t->data + start + ins_len, t->data + tail_from, tail_len);
    if (ins_len) memcpy(t->data + start, ins, ins_len);
    t->len = (size_t)(t->len - removed_len + ins_len);
    t->lines_valid = 0;
}

cdin_status text_pos_check(const text *t, uint64_t p)
{
    if (p > t->len) return CDIN_OUT_OF_RANGE;
    if (p == t->len) return CDIN_OK;
    if (utf8_inside(t->data, t->len, (size_t)p, NULL, NULL)) return CDIN_INVALID_POSITION;
    return CDIN_OK;
}

/* ---- line index (TEXT-020..023) --------------------------------------------- */
/* A terminator is LF, CRLF (one unit) or a CR not followed by LF. */
cdin_status text_build_lines(cdin_kernel *k, text *t)
{
    size_t nterm, n, cap_bytes = 0;
    uint64_t *arr;
    if (t->lines_valid) return CDIN_OK;
    nterm = line_term_count(t->data, t->len);
    if (!k_add(nterm, 1, &n)) return CDIN_LIMIT_EXCEEDED;
    if (!k_mul(n, sizeof(uint64_t), &cap_bytes)) return CDIN_LIMIT_EXCEEDED;
    (void)cap_bytes;
    if (t->lines_cap < n) {
        size_t ncap = n + n / 4 + 8;
        if (!k_mul(ncap, sizeof(uint64_t), &cap_bytes)) return CDIN_LIMIT_EXCEEDED;
        arr = t->lines ? k_realloc(k, t->lines, t->lines_cap * sizeof(uint64_t), cap_bytes)
                       : k_alloc(k, cap_bytes);
        if (!arr) return CDIN_NO_MEMORY;
        t->lines = arr;
        t->lines_cap = ncap;
    }
    line_fill_starts(t->data, t->len, t->lines);
    t->nlines = n;
    t->lines_valid = 1;
    return CDIN_OK;
}
