#!/usr/bin/env python3
"""Generate src/unicode_tables.c (pinned Unicode 15.1.0) for cdin-kernel.

PROVENANCE (read this before trusting the tables):
  The official UCD files (CaseFolding.txt, GraphemeBreakProperty.txt,
  emoji-data.txt, DerivedCoreProperties.txt) could not be downloaded in the
  environment where this generator was written (unicode.org was unreachable).
  The tables are therefore derived from two installed Python packages:

    * simple case folding  <- Python `unicodedata` (Unicode 15.1.0 in CPython 3.13):
        simple(cp) = str.casefold(cp) if that is one code point,
                     else str.lower(cp) if that is one code point,
                     else cp (no simple mapping)
      This reproduces CaseFolding.txt status C+S for the cases checked in
      tests/test_unicode.c, but it is NOT generated from CaseFolding.txt.
    * Grapheme_Cluster_Break, Extended_Pictographic and Indic_Conjunct_Break
      <- the third-party `regex` module. Its Unicode version is newer than
      15.1; to keep the pin honest, code points that are *unassigned* in
      Python's 15.1 database but assigned in `regex` are forced to class
      Other / not-ExtPict / InCB=None.

  Regenerate from the official files as soon as they are reachable and replace
  this script's data source (tools/README.md lists the exact files). The
  header comment of the generated file records which source was used.

Usage: python3 -I tools/gen_unicode.py > src/unicode_tables.c
"""
import sys
import unicodedata

import regex

assert unicodedata.unidata_version == "15.1.0", unicodedata.unidata_version

MAXCP = 0x110000
GCB_NAMES = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
             "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
GCB_ENUM = ["GCB_OTHER", "GCB_CR", "GCB_LF", "GCB_CONTROL", "GCB_EXTEND", "GCB_ZWJ", "GCB_RI",
            "GCB_PREPEND", "GCB_SPACINGMARK", "GCB_L", "GCB_V", "GCB_T", "GCB_LV", "GCB_LVT"]


def assigned_15_1(cp):
    return unicodedata.category(chr(cp)) != "Cn"


# Build one string of every non-surrogate code point and scan with finditer.
chars = []
index_of = []  # position in `chars` -> code point
for cp in range(MAXCP):
    if 0xD800 <= cp <= 0xDFFF:
        continue
    index_of.append(cp)
big = "".join(chr(cp) for cp in index_of)


def spans(pattern):
    """Yield (lo, hi) code point ranges (inclusive) matching a regex property."""
    rx = regex.compile(pattern)
    out = []
    for m in rx.finditer(big):
        lo_i, hi_i = m.start(), m.end() - 1
        # Spans are contiguous in `index_of` but may straddle the surrogate gap.
        lo, hi = index_of[lo_i], index_of[hi_i]
        run_lo = lo
        prev = lo
        for i in range(lo_i + 1, hi_i + 1):
            cur = index_of[i]
            if cur != prev + 1:
                out.append((run_lo, prev))
                run_lo = cur
            prev = cur
        out.append((run_lo, prev))
    return out


def merge(ranges_by_class):
    flat = []
    for cls, rs in ranges_by_class:
        for lo, hi in rs:
            flat.append((lo, hi, cls))
    flat.sort()
    return flat


def clip_unassigned(rs, keep_if_reserved):
    """Drop ranges' code points that are assigned in `regex` (newer) but Cn in 15.1."""
    out = []
    for lo, hi in rs:
        cur = None
        for cp in range(lo, hi + 1):
            keep = assigned_15_1(cp) or keep_if_reserved(cp)
            if keep:
                if cur is None:
                    cur = cp
                last = cp
            elif cur is not None:
                out.append((cur, last))
                cur = None
        if cur is not None:
            out.append((cur, last))
    return out


newer_assigned = regex.compile(r"\p{Assigned}")


def reserved_in_both(cp):
    # Unassigned in 15.1 AND unassigned in the newer database: keep the class
    # (e.g. reserved default-ignorable code points are Grapheme_Cluster_Break=Control).
    return newer_assigned.match(chr(cp)) is None


gcb_ranges = []
for i, name in enumerate(GCB_NAMES):
    if name == "Other":
        continue
    rs = spans(r"\p{GCB=%s}+" % name)
    rs = clip_unassigned(rs, reserved_in_both)
    gcb_ranges.append((i, rs))
gcb = merge(gcb_ranges)

ext = clip_unassigned(spans(r"\p{Extended_Pictographic}+"), lambda cp: True)  # reserved blocks stay
InCB = ["None", "Consonant", "Linker", "Extend"]
incb_ranges = []
for i, name in enumerate(InCB):
    if name == "None":
        continue
    rs = clip_unassigned(spans(r"\p{InCB=%s}+" % name), reserved_in_both)
    incb_ranges.append((i, rs))
incb = merge(incb_ranges)


def simple_fold(cp):
    c = chr(cp)
    f = c.casefold()
    if len(f) == 1:
        return ord(f)
    low = c.lower()
    if len(low) == 1:
        return ord(low)
    return cp


fold = []
for cp in range(MAXCP):
    if 0xD800 <= cp <= 0xDFFF:
        continue
    if not assigned_15_1(cp):
        continue
    t = simple_fold(cp)
    if t != cp:
        fold.append((cp, t))

out = []
w = out.append
w("/* GENERATED by tools/gen_unicode.py -- DO NOT EDIT.\n")
w(" * Pinned Unicode version: 15.1.0.\n")
w(" * Data source: Python unicodedata (case folding, assignment) and the `regex`\n")
w(" * module (GCB, Extended_Pictographic, InCB). NOT generated from the official\n")
w(" * UCD files; see the header of tools/gen_unicode.py and docs/limits-and-status.md. */\n")
w('#include "unicode_tables.h"\n\n')
w("const unsigned cdin_unicode_major = 15u, cdin_unicode_minor = 1u, cdin_unicode_patch = 0u;\n\n")
w("const cdin_range_cls cdin_gcb_table[] = {\n")
for lo, hi, cls in gcb:
    w("  {0x%X,0x%X,%s},\n" % (lo, hi, GCB_ENUM[cls]))
w("};\nconst unsigned cdin_gcb_table_len = %d;\n\n" % len(gcb))
w("const cdin_range_cls cdin_extpict_table[] = {\n")
for lo, hi in ext:
    w("  {0x%X,0x%X,1},\n" % (lo, hi))
w("};\nconst unsigned cdin_extpict_table_len = %d;\n\n" % len(ext))
w("const cdin_range_cls cdin_incb_table[] = {\n")
for lo, hi, cls in incb:
    w("  {0x%X,0x%X,%d},\n" % (lo, hi, cls))
w("};\nconst unsigned cdin_incb_table_len = %d;\n\n" % len(incb))
w("const cdin_fold_pair cdin_fold_table[] = {\n")
for a, b in fold:
    w("  {0x%X,0x%X},\n" % (a, b))
w("};\nconst unsigned cdin_fold_table_len = %d;\n" % len(fold))
sys.stdout.write("".join(out))
sys.stderr.write("gcb ranges %d, extpict %d, incb %d, fold pairs %d\n" % (len(gcb), len(ext), len(incb), len(fold)))
