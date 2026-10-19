/* Literal and byte search (SEARCH-*). The scan runs on a ref-counted immutable
 * text block outside the kernel lock (SEARCH-010): it never blocks mutation and
 * never sees partial state. Regex is not available in this release (ADR-0009
 * pending): REGEX mode returns UNSUPPORTED.
 *
 * Everything here is a function of the pinned block and the request; the api
 * wrapper (api/search_api.c) owns the lock, the version check and the pin. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "internal.h"

#define PAT_NL 0x0Au
#define CHUNK 65536u

/* A missing byte trigram proves that an exact byte-pattern cannot occur. The
 * index has no false negatives; a positive only means that a full scan may be
 * needed. The acquire load pairs with the index builder's release publication. */
static int indexed_pattern_maybe_present(const text *t, const uint8_t *p, size_t n)
{
    const uint8_t *bits;
    size_t i;
    if (!t || n < 3 || __atomic_load_n(&t->trigram_state, __ATOMIC_ACQUIRE) != 2u) return 1;
    bits = t->trigram_bits;
    for (i = 0; i + 2 < n; i++) {
        uint32_t tri = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | (uint32_t)p[i + 2];
        if ((bits[tri >> 3] & (uint8_t)(1u << (tri & 7u))) == 0) return 0;
    }
    return 1;
}

static int cancelled(const uint32_t *c)
{
    if (!c) return 0;
#if defined(__GNUC__)
    return __atomic_load_n(c, __ATOMIC_RELAXED) != 0;
#else
    return *(const volatile uint32_t *)c != 0;
#endif
}

/* Matches pat[0..m) at text position s (folded / newline-normalised code points). Returns end offset, or 0 for no match. */
static size_t match_cp(const uint8_t *d, size_t len, size_t s, const uint32_t *pat, size_t m,
                       uint32_t flags)
{
    size_t i = s, j;
    int ci = (flags & CDIN_SEARCH_CASE_INSENSITIVE) != 0;
    int nl = (flags & CDIN_SEARCH_ANY_NEWLINE) != 0;
    for (j = 0; j < m; j++) {
        uint32_t cp;
        size_t n;
        if (i >= len) return 0;
        n = utf8_decode(d + i, len - i, &cp);
        if (n == 0) return 0;                           /* raw byte never matches (SEARCH-040) */
        if (nl && pat[j] == PAT_NL) {
            if (cp == 0x0A) i += 1;
            else if (cp == 0x0D) i += (i + 1 < len && d[i + 1] == 0x0A) ? 2 : 1;
            else return 0;
            continue;
        }
        if (ci) cp = fold_simple(cp);
        if (cp != pat[j]) return 0;
        i += n;
    }
    return i;
}

int search_pattern_valid(const cdin_search_request *r)
{
    size_t i = 0;
    while (i < r->pattern_len) {
        size_t n = utf8_decode(r->pattern + i, r->pattern_len - i, NULL);
        if (n == 0) return 0;
        i += n;
    }
    return 1;
}

static void fail_details(cdin_search_result *out, cdin_status st, uint64_t a, uint64_t b)
{
    memset(&out->details, 0, sizeof out->details);
    out->details.struct_size = (uint32_t)sizeof out->details;
    if (st == CDIN_STALE_VERSION) { out->details.expected = a; out->details.current = b; }
    else if (st == CDIN_OUT_OF_RANGE || st == CDIN_INVALID_POSITION) { out->details.requested = a; out->details.length = b; }
}

void search_fail_details(cdin_search_result *out, cdin_status st, uint64_t a, uint64_t b)
{ fail_details(out, st, a, b); }

cdin_status search_prep_pattern(cdin_kernel *k, const cdin_search_request *r, uint32_t **out,size_t *m)
{
    size_t i = 0, cnt = 0, alloc_bytes;
    uint32_t *p;
    *out = NULL;
    *m = 0;
    if (!k_mul(r->pattern_len, sizeof *p, &alloc_bytes)) return CDIN_LIMIT_EXCEEDED;
    p = k_alloc(k, alloc_bytes);
    if (!p) return CDIN_NO_MEMORY;
    while (i < r->pattern_len) {
        uint32_t cp;
        size_t n = utf8_decode(r->pattern + i, r->pattern_len - i, &cp);
        if (n == 0) { k_free(k, p, alloc_bytes); return CDIN_PATTERN_INVALID; }
        i += n;
        if ((r->flags & CDIN_SEARCH_ANY_NEWLINE) && (cp == 0x0A || cp == 0x0D)) {
            if (cp == 0x0D && i < r->pattern_len && r->pattern[i] == 0x0A) i++;   /* CRLF is one newline */
            cp = PAT_NL;
        } else if (r->flags & CDIN_SEARCH_CASE_INSENSITIVE) {
            cp = fold_simple(cp);
        }
        p[cnt++] = cp;
    }
    *out = p;
    *m = cnt;
    return CDIN_OK;
}

cdin_status literal_run(const text *t, const cdin_search_request *r, const uint32_t *pat,size_t mcp, uint64_t step_limit, cdin_search_result *out)
{
    const uint8_t *d = t ? t->data : NULL;
    size_t tlen = t ? t->len : 0, plen = r->pattern_len, s = 0, scan_lo = 0, scan_hi = 0;
    const uint8_t *p0 = r->pattern;
    uint64_t from, bound, steps = 0, count = 0, last_start, last_end;
    int literal = r->mode == CDIN_SEARCH_LITERAL, fwd = r->direction == CDIN_SEARCH_FORWARD;
    int plain = pat == NULL, hit_limit = 0, hit_cancel = 0, ended = 0;

    (void)mcp;
    from = r->from;
    bound = r->bound == UINT64_MAX ? tlen : r->bound;
    last_start = bound;                       /* backward: the next match must end <= last_start */
    last_end = from;                          /* forward: the next match must start >= last_end */

    if (fwd) {
        s = (size_t)from;
        scan_lo = s;
        if (plain) {
            size_t cand_end = bound >= plen ? (size_t)bound - plen + 1 : 0; /* candidate starts are < cand_end */
            int filtered_no_match = 0;

            if (s < cand_end && r->cancel == NULL &&
                step_limit >= (uint64_t)(cand_end - s) &&
                !indexed_pattern_maybe_present(t, p0, plen)) {
                /* Every match would contain each pattern trigram. A missing one
                 * proves absence without inspecting candidate starts. */
                s = cand_end;
                ended = 1;
                filtered_no_match = 1;
            }

            if (!filtered_no_match) {
#if defined(__linux__)
                if (plen >= 4 && s < cand_end && r->cancel == NULL &&
                    step_limit >= (uint64_t)(cand_end - s)) {
                    /* Tuned libc search is retained as a fallback when the
                     * trigram filter cannot prove absence. */
                    while (s < cand_end) {
                        const uint8_t *q = (const uint8_t *)memmem(d + s, (size_t)(bound - s), p0, plen);
                        size_t offset;
                        if (!q) { steps += (uint64_t)(cand_end - s); s = cand_end; break; }
                        offset = (size_t)(q - (d + s));
                        steps += (uint64_t)offset + 1;
                        s += offset;
                        r->ranges[2 * count] = s;
                        r->ranges[2 * count + 1] = s + plen;
                        count++;
                        s += plen;
                        last_end = s;
                        if (count == r->limit) break;
                    }
                } else
#endif
                if (plen >= 4 && s < cand_end) {
                    /* Pick a selective byte using a small sample of this window. */
                    size_t freq[256] = {0};
                    size_t sample = (size_t)(bound - s);
                    size_t pivot = 0, best, i;
                    if (sample > 4096u) sample = 4096u;
                    for (i = 0; i < sample; i++) freq[d[s + i]]++;
                    best = freq[p0[0]];
                    for (i = 1; i < plen; i++) {
                        size_t f = freq[p0[i]];
                        if (f < best) { best = f; pivot = i; }
                    }
                    while (s < cand_end) {
                        uint64_t room = step_limit - steps;
                        size_t win = cand_end - s;
                        const uint8_t *q;
                        if (cancelled(r->cancel)) { hit_cancel = 1; break; }
                        if (room == 0) { hit_limit = 1; break; }
                        if (win > room) win = (size_t)room;
                        if (win > CHUNK) win = CHUNK;
                        q = memchr(d + s + pivot, p0[pivot], win);
                        if (!q) { steps += win; s += win; continue; }
                        steps += (uint64_t)(q - (d + s + pivot)) + 1;
                        s += (size_t)(q - (d + s + pivot));
                        if (memcmp(d + s, r->pattern, plen) == 0) {
                            r->ranges[2 * count] = s;
                            r->ranges[2 * count + 1] = s + plen;
                            count++;
                            s += plen;
                            last_end = s;
                            if (count == r->limit) break;
                        } else s++;
                    }
                } else {
                    /* libc memchr is generally optimal for short patterns. */
                    while (s < cand_end) {
                        uint64_t room = step_limit - steps;
                        size_t win = cand_end - s;
                        const uint8_t *q;
                        if (cancelled(r->cancel)) { hit_cancel = 1; break; }
                        if (room == 0) { hit_limit = 1; break; }
                        if (win > room) win = (size_t)room;
                        if (win > CHUNK) win = CHUNK;
                        q = memchr(d + s, p0[0], win);
                        if (!q) { steps += win; s += win; continue; }
                        steps += (uint64_t)(q - (d + s)) + 1;
                        s = (size_t)(q - d);
                        if (memcmp(d + s, r->pattern, plen) == 0) {
                            r->ranges[2 * count] = s;
                            r->ranges[2 * count + 1] = s + plen;
                            count++;
                            s += plen;
                            last_end = s;
                            if (count == r->limit) break;
                        } else s++;
                    }
                }
                ended = !hit_limit && !hit_cancel && count < r->limit;
            }
        } else {
            while (s < bound) {
                size_t e = 0;
                if ((steps & 0xFFFu) == 0 && cancelled(r->cancel)) { hit_cancel = 1; break; }
                if (steps >= step_limit) { hit_limit = 1; break; }
                steps++;
                if (!utf8_inside(d, tlen, s, NULL, NULL) &&
                    !((r->flags & CDIN_SEARCH_ANY_NEWLINE) && d[s] == 0x0A && s > 0 && d[s - 1] == 0x0D))
                    e = match_cp(d, tlen, s, pat, mcp, r->flags);
                if (e && e <= bound) {
                    r->ranges[2 * count] = s;
                    r->ranges[2 * count + 1] = e;
                    count++;
                    last_end = e;
                    s = e;
                    if (count == r->limit) break;
                    continue;
                }
                s++;
            }
            ended = !hit_limit && !hit_cancel && count < r->limit;
        }
        scan_hi = s < tlen ? s : tlen;
    } else {
        int64_t i = (int64_t)from;
        scan_hi = (size_t)from;
        for (;;) {
            size_t e = 0;
            if (i < 0) { ended = 1; break; }
            s = (size_t)i;
            if ((steps & 0xFFFu) == 0 && cancelled(r->cancel)) { hit_cancel = 1; break; }
            if (steps >= step_limit) { hit_limit = 1; break; }
            steps++;
            if (plain) {
                if (s + plen <= tlen && d[s] == p0[0] && memcmp(d + s, r->pattern, plen) == 0) e = s + plen;
            } else if (s < tlen && !utf8_inside(d, tlen, s, NULL, NULL) &&
                       !((r->flags & CDIN_SEARCH_ANY_NEWLINE) && d[s] == 0x0A && s > 0 && d[s - 1] == 0x0D)) {
                e = match_cp(d, tlen, s, pat, mcp, r->flags);
            }
            if (e && e <= bound && e <= last_start) {
                r->ranges[2 * count] = s;
                r->ranges[2 * count + 1] = e;
                count++;
                last_start = s;
                if (count == r->limit) { i--; break; }
            }
            i--;
        }
        scan_lo = i < 0 ? 0 : (size_t)i;
        s = i < 0 ? 0 : (size_t)i;            /* next start to examine */
    }

    if (hit_limit || hit_cancel) {
        /* Only `details` is written on a failed search. Without want_partial
         * the matches are not published (not even count = 0, which would
         * itself be a write): the caller's scratch array may hold debris
         * beyond the old count, but count itself was never changed. */
        memset(&out->details, 0, sizeof out->details);
        out->details.struct_size = (uint32_t)sizeof out->details;
        if (hit_limit) out->details.limit_id = CDIN_LIMIT_SEARCH_STEPS;
        if (r->flags & CDIN_SEARCH_WANT_PARTIAL) {
            out->partial = 1;
            out->count = count;
            out->steps = steps;
            out->truncated = 0;
            out->exhausted = 0;
            if (fwd) {
                out->next_from = s;
                out->next_bound = bound;
            } else {
                out->next_from = s;                    /* continue scanning downward */
                out->next_bound = count ? last_start : bound;
            }
        }
        return hit_limit ? CDIN_LIMIT_EXCEEDED : CDIN_CANCELLED;
    }

    {   /* SEARCH-042: did the scanned window contain raw bytes? */
        int raw = 0;
        int valid_known = t && __atomic_load_n(&t->trigram_state, __ATOMIC_ACQUIRE) == 2u &&
                          t->utf8_has_raw == 0u;
        if (literal && scan_hi > scan_lo && !valid_known) {
            size_t q = scan_lo;
            while (q < scan_hi && q < tlen) {
                size_t n = utf8_decode(d + q, tlen - q, NULL);
                if (n == 0) { raw = 1; break; }
                q += n;
            }
        }
        out->skipped_raw = raw;
    }

    out->count = count;
    out->steps = steps;
    out->truncated = count == r->limit;
    out->partial = 0;
    out->exhausted = ended;
    if (fwd) {
        out->next_from = count ? last_end : from;
        out->next_bound = bound;
    } else {
        out->next_from = ended ? 0 : s;
        out->next_bound = count ? last_start : bound;
    }
    return CDIN_OK;
}
