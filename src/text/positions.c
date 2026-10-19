/* Byte-offset / code-point-index conversion, pure on (bytes, len) (POS-031).
 *
 * Each unit is one code point: one well-formed sequence, or one raw byte when
 * the classifier says the bytes cannot start or continue a sequence. Crossing
 * a raw unit sets the CDIN_CONV_TOUCHED_RAW flag, which the caller surfaces. */
#include "internal.h"

cdin_status cp_index_at(const uint8_t *d, size_t len, uint64_t offset, uint64_t *out, uint32_t *flags)
{
    size_t i = 0;
    uint64_t cp = 0;
    uint32_t f = 0;
    if (!out) return CDIN_INVALID_ARGUMENT;
    if (offset > (uint64_t)len) return CDIN_OUT_OF_RANGE;
    while (i < offset) {
        size_t n = utf8_decode(d + i, len - i, NULL);
        if (n == 0) { n = 1; f |= CDIN_CONV_TOUCHED_RAW; }
        i += n;
        cp++;
    }
    *out = cp;
    if (flags) *flags = f;
    return CDIN_OK;
}

cdin_status cp_offset_of(const uint8_t *d, size_t len, uint64_t index, uint64_t *out, uint32_t *flags)
{
    size_t i = 0;
    uint64_t cp = 0;
    uint32_t f = 0;
    if (!out) return CDIN_INVALID_ARGUMENT;
    while (cp < index && i < len) {
        size_t n = utf8_decode(d + i, len - i, NULL);
        if (n == 0) { n = 1; f |= CDIN_CONV_TOUCHED_RAW; }
        i += n;
        cp++;
    }
    if (cp < index) return CDIN_OUT_OF_RANGE;
    *out = i;
    if (flags) *flags = f;
    return CDIN_OK;
}
