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
    t->trigram_bits = NULL;
    t->trigram_state = 0;
    t->utf8_has_raw = 0;
    return t;
}

void text_release(cdin_kernel *k, text *t)
{
    if (!t) return;
    if (--t->refs > 0) return;
    k_free(k, t->lines, t->lines_cap * sizeof(uint64_t));
    k_free(k, t->trigram_bits, TEXT_TRIGRAM_INDEX_BYTES);
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

void text_splice(cdin_kernel *k, text *t, uint64_t start, uint64_t removed_len, const uint8_t *ins, size_t ins_len)
{
    size_t tail_from = (size_t)(start + removed_len);
    size_t tail_len = t->len - tail_from;
    if (tail_len && ins_len != removed_len)
        memmove(t->data + start + ins_len, t->data + tail_from, tail_len);
    if (ins_len) memcpy(t->data + start, ins, ins_len);
    t->len = (size_t)(t->len - removed_len + ins_len);
    t->lines_valid = 0;
    /* The content changed. No active reader can share this block (COW ensures
     * unique ownership before splice), so invalidate and reclaim its cache. */
    k_free(k, t->trigram_bits, TEXT_TRIGRAM_INDEX_BYTES);
    t->trigram_bits = NULL;
    t->utf8_has_raw = 0;
    __atomic_store_n(&t->trigram_state, 0u, __ATOMIC_RELEASE);
}

/* Start building only while the caller owns the kernel lock. The pointer is
 * not read by searches until state becomes 2 with release/acquire ordering. */
int text_search_index_begin(cdin_kernel *k, text *t)
{
    uint8_t *bits;
    if (__atomic_load_n(&t->trigram_state, __ATOMIC_ACQUIRE) != 0u) return 0;
    bits = (uint8_t *)k_alloc(k, TEXT_TRIGRAM_INDEX_BYTES);
    if (!bits) return 0;
    t->trigram_bits = bits;
    __atomic_store_n(&t->trigram_state, 1u, __ATOMIC_RELEASE);
    return 1;
}

/* Build a precise presence bitset for every 3-byte sequence while also
 * classifying UTF-8 validity. It runs on a pinned immutable block, outside the
 * kernel lock; once ready, absent trigrams can reject no-match queries in O(m). */
void text_search_index_build(text *t)
{
    uint8_t *bits = t->trigram_bits;
    const uint8_t *d = t->data;
    size_t i = 0;
    uint32_t has_non_ascii = 0, has_raw = 0;
    memset(bits, 0, TEXT_TRIGRAM_INDEX_BYTES);

    /* Build the bitset in eight-byte chunks. The high-bit test lets the common
     * ASCII case avoid a UTF-8 decoder branch for every byte. */
    while (t->len - i >= sizeof(uint64_t)) {
        uint64_t word;
        size_t j;
        memcpy(&word, d + i, sizeof word); /* safe for unaligned input */
        if (word & UINT64_C(0x8080808080808080)) has_non_ascii = 1;
        for (j = 0; j < sizeof(uint64_t); ++j) {
            size_t pos = i + j;
            if (t->len - pos >= 3) {
                uint32_t tri = ((uint32_t)d[pos] << 16) |
                               ((uint32_t)d[pos + 1] << 8) |
                               (uint32_t)d[pos + 2];
                bits[tri >> 3] |= (uint8_t)(1u << (tri & 7u));
            }
        }
        i += sizeof(uint64_t);
    }
    for (; i < t->len; ++i) {
        if (t->len - i >= 3) {
            uint32_t tri = ((uint32_t)d[i] << 16) |
                           ((uint32_t)d[i + 1] << 8) |
                           (uint32_t)d[i + 2];
            bits[tri >> 3] |= (uint8_t)(1u << (tri & 7u));
        }
        if (d[i] >= 0x80u) has_non_ascii = 1;
    }

    /* ASCII is always valid UTF-8. Only spend a full decode pass when the
     * document actually contains bytes with the high bit set. */
    if (has_non_ascii) {
        i = 0;
        while (i < t->len) {
            size_t n = utf8_decode(d + i, t->len - i, NULL);
            if (n == 0) { has_raw = 1; break; }
            i += n;
        }
    }
    t->utf8_has_raw = has_raw;
    __atomic_store_n(&t->trigram_state, 2u, __ATOMIC_RELEASE);
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
