/* Public wrapper for cdin_search (SEARCH-*).
 *
 * The wrapper owns: argument validation, the lock, the version check, the
 * position checks, pattern preparation, pinning the block (refs++), releasing
 * it afterwards and filling result.version. The scan itself is the pure
 * literal_run() in search/literal.c — it never touches the kernel, so a
 * running search cannot block a mutation, and a mutation cannot disturb a
 * running search (SEARCH-010). */
#include "internal.h"

#define SEARCH_INDEX_SAMPLE_COUNT 16u
#define SEARCH_INDEX_SAMPLE_WIDTH 65536u

/* Avoid paying the one-time index-build cost for common searches that already
 * match early in a large document. This sampling is only a performance hint: a
 * miss never changes search semantics and simply permits index construction. */
static int sampled_pattern_present(const text *t, uint64_t from, uint64_t cand_end,
                                   const uint8_t *pattern, size_t pattern_len)
{
    uint64_t span = cand_end - from;
    uint64_t width = span < SEARCH_INDEX_SAMPLE_WIDTH ? span : SEARCH_INDEX_SAMPLE_WIDTH;
    uint64_t spread = span - width;
    uint32_t sample_count = spread == 0 ? 1u : SEARCH_INDEX_SAMPLE_COUNT;
    uint64_t denom = sample_count > 1u ? (uint64_t)(sample_count - 1u) : 1u;
    uint32_t sample;
    for (sample = 0; sample < sample_count; sample++) {
        uint64_t start = from + (spread / denom) * sample + ((spread % denom) * sample) / denom;
        uint64_t candidates = cand_end - start;
        size_t candidate_bytes = (size_t)(candidates < width ? candidates : width);
        size_t hay_len = candidate_bytes + pattern_len - 1u;
        const uint8_t *base = t->data + (size_t)start;
        const uint8_t *q;
        if (hay_len < pattern_len) continue;
        q = (const uint8_t *)memchr(base, pattern[0], hay_len);
        while (q) {
            size_t off = (size_t)(q - base);
            if (hay_len - off >= pattern_len && memcmp(q, pattern, pattern_len) == 0) return 1;
            if (off + 1u >= hay_len) break;
            q = (const uint8_t *)memchr(base + off + 1u, pattern[0], hay_len - off - 1u);
        }
    }
    return 0;
}

cdin_status cdin_search(cdin_kernel *k, const cdin_search_request *r, cdin_search_result *out)
{
    cdin_status st;
    text *t;
    cdin_version ver;
    uint32_t *pat = NULL;
    size_t mcp = 0;
    uint64_t step_limit;
    int literal, may_build_search_index = 0, build_search_index = 0;

    if (!k || !out || !r) return CDIN_INVALID_ARGUMENT;
    if (out->struct_size < sizeof *out) return CDIN_INVALID_ARGUMENT;
    st = check_struct(r, sizeof *r, sizeof *r);
    if (st != CDIN_OK) return st;
    if (r->mode == CDIN_SEARCH_REGEX) return CDIN_UNSUPPORTED;
    if (r->mode != CDIN_SEARCH_LITERAL && r->mode != CDIN_SEARCH_BYTES) return CDIN_INVALID_ARGUMENT;
    if (r->direction != CDIN_SEARCH_FORWARD && r->direction != CDIN_SEARCH_BACKWARD) return CDIN_INVALID_ARGUMENT;
    if (r->flags & ~(CDIN_SEARCH_CASE_INSENSITIVE | CDIN_SEARCH_ANY_NEWLINE | CDIN_SEARCH_WANT_PARTIAL))
        return CDIN_UNSUPPORTED;
    literal = r->mode == CDIN_SEARCH_LITERAL;
    if (!literal && (r->flags & (CDIN_SEARCH_CASE_INSENSITIVE | CDIN_SEARCH_ANY_NEWLINE)))
        return CDIN_INVALID_ARGUMENT;
    if (r->pattern_len == 0 || !r->pattern) return CDIN_INVALID_ARGUMENT;
    if (r->limit == 0 || r->limit > r->ranges_capacity || !r->ranges) return CDIN_INVALID_ARGUMENT;
    if (literal && !search_pattern_valid(r)) return CDIN_PATTERN_INVALID;

    k_lock(k);
    st = src_lookup(k, r->src, &t, &ver);
    if (st != CDIN_OK) { k_unlock(k); return st; }
    if (r->expected_version != CDIN_ANY_VERSION && r->expected_version != ver) {
        k_unlock(k);
        search_fail_details(out, CDIN_STALE_VERSION, r->expected_version, ver);
        return CDIN_STALE_VERSION;
    }
    if (r->from > t->len || (r->bound != UINT64_MAX && r->bound > t->len)) {
        k_unlock(k);
        search_fail_details(out, CDIN_OUT_OF_RANGE, r->from > t->len ? r->from : r->bound, t->len);
        return CDIN_OUT_OF_RANGE;
    }
    if (literal) {
        cdin_status a = text_pos_check(t, r->from), b = text_pos_check(t, r->bound == UINT64_MAX ? t->len : r->bound);
        if (a != CDIN_OK || b != CDIN_OK) {
            k_unlock(k);
            search_fail_details(out, CDIN_INVALID_POSITION, a != CDIN_OK ? r->from : r->bound, t->len);
            return CDIN_INVALID_POSITION;
        }
        if (r->flags & (CDIN_SEARCH_CASE_INSENSITIVE | CDIN_SEARCH_ANY_NEWLINE)) {
            st = search_prep_pattern(k, r, &pat, &mcp);
            if (st != CDIN_OK) { k_unlock(k); return st; }
        }
    }

    step_limit = r->step_limit ? r->step_limit : k->default_steps;
    /* Large plain forward searches are candidates for a reusable exact trigram
     * index. First pin the block and unlock; sample the immutable bytes before
     * paying the one-time build cost so common early matches stay fast. */
    if (t->len >= TEXT_SEARCH_INDEX_MIN_BYTES && r->pattern_len >= 3 &&
        r->direction == CDIN_SEARCH_FORWARD && r->cancel == NULL &&
        (r->flags & (CDIN_SEARCH_CASE_INSENSITIVE | CDIN_SEARCH_ANY_NEWLINE)) == 0) {
        uint64_t search_bound = r->bound == UINT64_MAX ? (uint64_t)t->len : r->bound;
        if (search_bound >= (uint64_t)r->pattern_len) {
            uint64_t cand_end = search_bound - (uint64_t)r->pattern_len + 1u;
            if (r->from < cand_end && step_limit >= cand_end - r->from)
                may_build_search_index = 1;
        }
    }

    t->refs++;                                /* from here on the scan only reads this immutable block */
    k_unlock(k);

    if (may_build_search_index &&
        __atomic_load_n(&t->trigram_state, __ATOMIC_ACQUIRE) == 0u) {
        uint64_t search_bound = r->bound == UINT64_MAX ? (uint64_t)t->len : r->bound;
        uint64_t cand_end = search_bound - (uint64_t)r->pattern_len + 1u;
        if (!sampled_pattern_present(t, r->from, cand_end, r->pattern, r->pattern_len)) {
            k_lock(k);
            build_search_index = text_search_index_begin(k, t);
            k_unlock(k);
        }
    }
    if (build_search_index) text_search_index_build(t);
    st = literal_run(t, r, pat, mcp, step_limit, out);

    k_lock(k);
    text_release(k, t);
    k_free(k, pat, r->pattern_len * sizeof *pat);
    k_unlock(k);

    /* version is part of a published result: full success, or a partial one
     * (want_partial). Any other failure leaves the result untouched but may
     * fill `details`. */
    if (st == CDIN_OK || (r->flags & CDIN_SEARCH_WANT_PARTIAL)) out->version = ver;
    return st;
}
