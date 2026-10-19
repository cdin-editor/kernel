/* Public wrapper for cdin_search (SEARCH-*).
 *
 * The wrapper owns: argument validation, the lock, the version check, the
 * position checks, pattern preparation, pinning the block (refs++), releasing
 * it afterwards and filling result.version. The scan itself is the pure
 * literal_run() in search/literal.c — it never touches the kernel, so a
 * running search cannot block a mutation, and a mutation cannot disturb a
 * running search (SEARCH-010). */
#include "internal.h"

cdin_status cdin_search(cdin_kernel *k, const cdin_search_request *r, cdin_search_result *out)
{
    cdin_status st;
    text *t;
    cdin_version ver;
    uint32_t *pat = NULL;
    size_t mcp = 0;
    uint64_t step_limit;
    int literal;

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
    t->refs++;                                /* from here on the scan only reads this immutable block */
    k_unlock(k);

    step_limit = r->step_limit ? r->step_limit : k->default_steps;
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
