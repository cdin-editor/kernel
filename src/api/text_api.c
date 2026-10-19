/* Public wrappers for the read-only text group. The heavy lifting is either a
 * field read, or a pure helper in text/ (lines.c, positions.c, graphemes.c);
 * here: only lock, resolve, call, unlock. All of these work on documents and
 * snapshots alike through cdin_source (TEXT-005/006). */
#include "internal.h"

static cdin_status version_check(cdin_version have, cdin_version want)
{
    if (want != CDIN_ANY_VERSION && want != have) return CDIN_STALE_VERSION;
    return CDIN_OK;
}

cdin_status cdin_text_version(cdin_kernel *k, cdin_source src, cdin_version *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    *out = v;
    k_unlock(k);
    return CDIN_OK;
}

cdin_status cdin_text_len(cdin_kernel *k, cdin_source src, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    *out = t->len;
    k_unlock(k);
    return CDIN_OK;
}

cdin_status cdin_text_read(cdin_kernel *k, cdin_source src, uint64_t start, uint64_t end,
                           uint8_t *buf, size_t bufsz, size_t *out_len, uint32_t *flags)
{
    text *t; cdin_version v; cdin_status st;
    size_t n;
    if (!k || !out_len) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (start > end || end > t->len) { k_unlock(k); return CDIN_OUT_OF_RANGE; }
    n = (size_t)(end - start);
    if (buf == NULL) {
        *out_len = n;
    } else if (bufsz < n) {
        *out_len = n;
        k_unlock(k);
        return CDIN_BUFFER_TOO_SMALL;
    } else {
        if (n) memcpy(buf, t->data + start, n);
        *out_len = n;
    }
    if (flags) {
        uint32_t f = 0;
        if (start < t->len && utf8_inside(t->data, t->len, (size_t)start, NULL, NULL)) f |= CDIN_READ_STARTS_MID_SEQUENCE;
        if (end < t->len && utf8_inside(t->data, t->len, (size_t)end, NULL, NULL)) f |= CDIN_READ_ENDS_MID_SEQUENCE;
        *flags = f;
    }
    k_unlock(k);
    return CDIN_OK;
}

cdin_status cdin_text_serialize(cdin_kernel *k, cdin_source src, uint8_t *buf, size_t bufsz, size_t *out_len)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out_len) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    *out_len = t->len;
    if (buf) {
        if (bufsz < t->len) { k_unlock(k); return CDIN_BUFFER_TOO_SMALL; }
        if (t->len) memcpy(buf, t->data, t->len);
    }
    k_unlock(k);
    return CDIN_OK;
}

cdin_status cdin_text_check_position(cdin_kernel *k, cdin_source src, cdin_version expected_version, uint64_t offset)
{
    text *t; cdin_version v; cdin_status st;
    if (!k) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    st = version_check(v, expected_version);
    if (st == CDIN_OK) st = text_pos_check(t, offset);
    k_unlock(k);
    return st;
}

cdin_status cdin_text_region_at(cdin_kernel *k, cdin_source src, uint64_t offset,
                                uint64_t *start, uint64_t *end, int *is_raw)
{
    text *t; cdin_version v; cdin_status st;
    size_t lo, hi;
    int cls;
    if (!k || !start || !end || !is_raw) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (offset >= t->len) { k_unlock(k); return CDIN_OUT_OF_RANGE; }
    cls = utf8_byte_valid(t->data, t->len, (size_t)offset);
    lo = (size_t)offset;
    while (lo > 0 && utf8_byte_valid(t->data, t->len, lo - 1) == cls) lo--;
    hi = (size_t)offset + 1;
    while (hi < t->len && utf8_byte_valid(t->data, t->len, hi) == cls) hi++;
    *start = lo; *end = hi; *is_raw = !cls;
    k_unlock(k);
    return CDIN_OK;
}

/* ---- lines ---------------------------------------------------------------------- */
cdin_status cdin_text_line_count(cdin_kernel *k, cdin_source src, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    st = text_build_lines(k, t);
    if (st == CDIN_OK) *out = t->nlines;
    k_unlock(k);
    return st;
}

cdin_status cdin_text_line_start(cdin_kernel *k, cdin_source src, uint64_t line, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    st = text_build_lines(k, t);
    if (st == CDIN_OK) {
        if (line >= t->nlines) st = CDIN_OUT_OF_RANGE;
        else *out = t->lines[line];
    }
    k_unlock(k);
    return st;
}

cdin_status cdin_text_line_from_offset(cdin_kernel *k, cdin_source src, uint64_t offset,
                                       uint64_t *line, uint64_t *col)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !line || !col) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (offset > t->len) { k_unlock(k); return CDIN_OUT_OF_RANGE; }
    st = text_build_lines(k, t);
    if (st == CDIN_OK) line_of(t->lines, t->nlines, offset, line, col);
    k_unlock(k);
    return st;
}

cdin_status cdin_text_eol_info(cdin_kernel *k, cdin_source src, cdin_eol_info *out)
{
    text *t; cdin_version v; cdin_status st;
    cdin_eol_info r;
    uint64_t lf, crlf, cr;
    int unterm;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    if (out->struct_size < sizeof(cdin_eol_info)) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    memset(&r, 0, sizeof r);
    eol_scan(t->data, t->len, &lf, &crlf, &cr, &unterm);
    r.lf = lf; r.crlf = crlf; r.cr = cr;
    r.final_line_unterminated = unterm;
    r.mixed = ((r.lf > 0) + (r.crlf > 0) + (r.cr > 0)) > 1;
    r.struct_size = (uint32_t)sizeof r;
    *out = r;
    k_unlock(k);
    return CDIN_OK;
}

/* ---- code points ----------------------------------------------------------------- */
cdin_status cdin_text_byte_to_codepoint(cdin_kernel *k, cdin_source src, uint64_t offset,
                                        uint64_t *out, uint32_t *flags)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    st = text_pos_check(t, offset);
    if (st == CDIN_OK) st = cp_index_at(t->data, t->len, offset, out, flags);
    k_unlock(k);
    return st;
}

cdin_status cdin_text_codepoint_to_byte(cdin_kernel *k, cdin_source src, uint64_t index,
                                        uint64_t *out, uint32_t *flags)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    st = cp_offset_of(t->data, t->len, index, out, flags);
    k_unlock(k);
    return st;
}

cdin_status cdin_text_codepoint_next(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (offset >= t->len) st = CDIN_OUT_OF_RANGE;
    else *out = unit_end(t->data, t->len, unit_start(t->data, t->len, (size_t)offset));
    k_unlock(k);
    return st;
}

cdin_status cdin_text_codepoint_prev(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (offset == 0 || offset > t->len) st = CDIN_OUT_OF_RANGE;
    else *out = unit_start(t->data, t->len, (size_t)offset - 1);
    k_unlock(k);
    return st;
}

/* ---- grapheme clusters (POS-030, POS-032, POS-033) ---------------------------- */
cdin_status cdin_text_grapheme_next(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (offset >= t->len) { k_unlock(k); return CDIN_OUT_OF_RANGE; }
    *out = gc_next_boundary(t->data, t->len, offset);
    k_unlock(k);
    return CDIN_OK;
}

cdin_status cdin_text_grapheme_prev(cdin_kernel *k, cdin_source src, uint64_t offset, uint64_t *out)
{
    text *t; cdin_version v; cdin_status st;
    if (!k || !out) return CDIN_INVALID_ARGUMENT;
    st = text_qbegin(k, src, &t, &v);
    if (st != CDIN_OK) return st;
    if (offset == 0 || offset > t->len) { k_unlock(k); return CDIN_OUT_OF_RANGE; }
    *out = gc_prev_boundary(t->data, t->len, offset);
    k_unlock(k);
    return CDIN_OK;
}
