/* Table layouts for the generated src/unicode_tables.c. Internal. */
#ifndef CDIN_UNICODE_TABLES_H
#define CDIN_UNICODE_TABLES_H

#include <stdint.h>

typedef enum {
    GCB_OTHER = 0, GCB_CR, GCB_LF, GCB_CONTROL, GCB_EXTEND, GCB_ZWJ, GCB_RI,
    GCB_PREPEND, GCB_SPACINGMARK, GCB_L, GCB_V, GCB_T, GCB_LV, GCB_LVT
} gcb_class;

typedef struct cdin_range_cls { uint32_t lo, hi; uint8_t cls; } cdin_range_cls;
typedef struct cdin_fold_pair { uint32_t from, to; } cdin_fold_pair;

extern const unsigned cdin_unicode_major, cdin_unicode_minor, cdin_unicode_patch;
extern const cdin_range_cls cdin_gcb_table[];
extern const unsigned cdin_gcb_table_len;
extern const cdin_range_cls cdin_extpict_table[];
extern const unsigned cdin_extpict_table_len;
extern const cdin_range_cls cdin_incb_table[];
extern const unsigned cdin_incb_table_len;
extern const cdin_fold_pair cdin_fold_table[];
extern const unsigned cdin_fold_table_len;

#endif
