/* Extended grapheme cluster boundaries (POS-030, POS-032, POS-033).
 *
 * UAX #29 rules GB1-GB13 and GB9c of Unicode 15.1, driven by the pinned
 * generated tables (gb_of, extpict_of, incb_of). A raw byte is its own cluster
 * (POS-033): no boundary rule may join anything to it. The state machine walks
 * clusters forward; the boundary queries recover the cluster containing an
 * arbitrary offset, which may be inside one. */
#include "internal.h"

/* ---- property lookups ---------------------------------------------------------- */
static int range_lookup(const cdin_range_cls *t, unsigned n, uint32_t cp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].lo) hi = mid;
        else if (cp > t[mid].hi) lo = mid + 1;
        else return t[mid].cls;
    }
    return -1;
}

static gcb_class gcb_of(uint32_t cp)
{
    int c = range_lookup(cdin_gcb_table, cdin_gcb_table_len, cp);
    return c < 0 ? GCB_OTHER : (gcb_class)c;
}

static int extpict_of(uint32_t cp) { return range_lookup(cdin_extpict_table, cdin_extpict_table_len, cp) >= 0; }

static int incb_of(uint32_t cp)
{
    int c = range_lookup(cdin_incb_table, cdin_incb_table_len, cp);
    return c < 0 ? 0 : c;
}

/* ---- cluster state machine ------------------------------------------------------ */
typedef struct gc_state {
    gcb_class prev;
    int prev_raw;
    unsigned ri_run;      /* consecutive Regional_Indicator code points ending at prev */
    int ep;               /* 0 none, 1 ExtPict Extend*, 2 ExtPict Extend* ZWJ */
    int incb;             /* 0 none, 1 consonant [extend]*, 2 ... linker ... */
} gc_state;

static void gc_init(gc_state *s, uint32_t cp, int raw)
{
    s->prev_raw = raw;
    s->prev = raw ? GCB_CONTROL : gcb_of(cp);
    s->ri_run = (!raw && s->prev == GCB_RI) ? 1u : 0u;
    s->ep = (!raw && extpict_of(cp)) ? 1 : 0;
    s->incb = (!raw && incb_of(cp) == 1) ? 1 : 0;
}

/* Returns 1 if there is NO boundary between the state's last unit and c. */
static int gc_join(gc_state *s, uint32_t cp, int raw)
{
    gcb_class c = raw ? GCB_CONTROL : gcb_of(cp);
    int join = 0;
    int c_ext = raw ? 0 : extpict_of(cp);
    int c_incb = raw ? 0 : incb_of(cp);

    if (s->prev_raw || raw) {
        join = 0;
    } else if (s->prev == GCB_CR && c == GCB_LF) {
        join = 1;                                                        /* GB3 */
    } else if (s->prev == GCB_CONTROL || s->prev == GCB_CR || s->prev == GCB_LF ||
               c == GCB_CONTROL || c == GCB_CR || c == GCB_LF) {
        join = 0;                                                        /* GB4, GB5 */
    } else if (s->prev == GCB_L && (c == GCB_L || c == GCB_V || c == GCB_LV || c == GCB_LVT)) {
        join = 1;                                                        /* GB6 */
    } else if ((s->prev == GCB_LV || s->prev == GCB_V) && (c == GCB_V || c == GCB_T)) {
        join = 1;                                                        /* GB7 */
    } else if ((s->prev == GCB_LVT || s->prev == GCB_T) && c == GCB_T) {
        join = 1;                                                        /* GB8 */
    } else if (c == GCB_EXTEND || c == GCB_ZWJ) {
        join = 1;                                                        /* GB9 */
    } else if (c == GCB_SPACINGMARK) {
        join = 1;                                                        /* GB9a */
    } else if (s->prev == GCB_PREPEND) {
        join = 1;                                                        /* GB9b */
    } else if (c_incb == 1 && s->incb == 2) {
        join = 1;                                                        /* GB9c */
    } else if (c_ext && s->ep == 2) {
        join = 1;                                                        /* GB11 */
    } else if (s->prev == GCB_RI && c == GCB_RI && (s->ri_run & 1u)) {
        join = 1;                                                        /* GB12, GB13 */
    }

    /* update state with c */
    if (raw) {
        s->prev_raw = 1; s->prev = GCB_CONTROL; s->ri_run = 0; s->ep = 0; s->incb = 0;
        return join;
    }
    s->ri_run = (c == GCB_RI) ? (join ? s->ri_run + 1u : 1u) : 0u;
    if (c_ext) s->ep = 1;
    else if (s->ep == 1 && c == GCB_EXTEND) s->ep = 1;
    else if (s->ep == 1 && c == GCB_ZWJ) s->ep = 2;
    else s->ep = 0;
    if (c_incb == 1) s->incb = 1;
    else if (c_incb == 2) s->incb = (s->incb >= 1) ? 2 : 0;
    else if (c_incb == 3) s->incb = (s->incb >= 1) ? s->incb : 0;
    else s->incb = 0;
    s->prev = c;
    s->prev_raw = 0;
    return join;
}

size_t gc_cluster_end(const uint8_t *d, size_t len, size_t start)
{
    gc_state st;
    uint32_t cp = 0;
    size_t n, pos;
    if (start >= len) return len;
    n = utf8_decode(d + start, len - start, &cp);
    if (n == 0) return start + 1;                 /* raw byte: its own cluster */
    gc_init(&st, cp, 0);
    pos = start + n;
    while (pos < len) {
        int raw;
        n = utf8_decode(d + pos, len - pos, &cp);
        raw = (n == 0);
        if (!gc_join(&st, cp, raw)) break;
        pos += raw ? 1 : n;
    }
    return pos;
}

/* A position that is certainly a cluster boundary and <= p (after a
 * Control/CR/LF/raw unit, or 0). */
static size_t gc_safe_start(const uint8_t *d, size_t len, size_t p)
{
    size_t pos = p;
    while (pos > 0) {
        size_t q = pos - 1, us = unit_start(d, len, q);
        uint32_t cp;
        size_t n = utf8_decode(d + us, len - us, &cp);
        if (n == 0) return pos;                    /* raw unit: boundary after it */
        {
            gcb_class c = gcb_of(cp);
            if (c == GCB_CR && pos < len && d[pos] == 0x0A) { pos = us; continue; } /* GB3 */
            if (c == GCB_CONTROL || c == GCB_CR || c == GCB_LF) return pos;
        }
        pos = us;
    }
    return 0;
}

size_t gc_next_boundary(const uint8_t *d, size_t len, uint64_t offset)
{
    size_t pos, e;
    if (offset >= (uint64_t)len) return len;
    pos = gc_safe_start(d, len, (size_t)offset);
    for (;;) {
        e = gc_cluster_end(d, len, pos);
        if (e > offset) break;
        pos = e;
    }
    return e;
}

size_t gc_prev_boundary(const uint8_t *d, size_t len, uint64_t offset)
{
    size_t pos, p0;
    if (offset == 0 || offset > (uint64_t)len) return 0;
    p0 = unit_start(d, len, (size_t)offset - 1);
    pos = gc_safe_start(d, len, p0);
    for (;;) {
        size_t e = gc_cluster_end(d, len, pos);
        if (e >= offset) break;
        pos = e;
    }
    return pos;
}
