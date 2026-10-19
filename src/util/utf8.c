/* UTF-8 classification (TEXT-010..012).
 *
 * Regions: bytes are partitioned into well-formed sequences ("valid") and
 * everything else ("raw"). Classification is a pure function of the bytes and
 * is evaluated locally: a continuation byte can only belong to the sequence of
 * the lead byte found at most three bytes before it, and a lead byte always
 * starts a fresh decode, so local and global decoding agree. */
#include "internal.h"

size_t utf8_decode(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint8_t b;
    uint32_t c;
    if (n == 0) return 0;
    b = s[0];
    if (b < 0x80) { if (cp) *cp = b; return 1; }
    if (b >= 0xC2 && b <= 0xDF) {
        if (n < 2 || (s[1] & 0xC0) != 0x80) return 0;
        c = ((uint32_t)(b & 0x1F) << 6) | (s[1] & 0x3F);
        if (cp) *cp = c;
        return 2;
    }
    if (b >= 0xE0 && b <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (b == 0xE0 && s[1] < 0xA0) return 0;      /* overlong */
        if (b == 0xED && s[1] > 0x9F) return 0;      /* surrogates */
        c = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (cp) *cp = c;
        return 3;
    }
    if (b >= 0xF0 && b <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (b == 0xF0 && s[1] < 0x90) return 0;      /* overlong */
        if (b == 0xF4 && s[1] > 0x8F) return 0;      /* > U+10FFFF */
        c = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (cp) *cp = c;
        return 4;
    }
    return 0;
}

int utf8_inside(const uint8_t *d, size_t len, size_t p, size_t *seq_start, size_t *seq_len)
{
    size_t k;
    if (p >= len || (d[p] & 0xC0) != 0x80) return 0;
    for (k = 1; k <= 3 && k <= p; k++) {
        uint8_t b = d[p - k];
        if ((b & 0xC0) == 0x80) continue;           /* more continuation bytes */
        if (b >= 0xC2) {
            size_t n = utf8_decode(d + (p - k), len - (p - k), NULL);
            if (n > k) {
                if (seq_start) *seq_start = p - k;
                if (seq_len) *seq_len = n;
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

int utf8_byte_valid(const uint8_t *d, size_t len, size_t i)
{
    if (utf8_decode(d + i, len - i, NULL) != 0) return 1;
    return utf8_inside(d, len, i, NULL, NULL);
}

int utf8_all_valid(const uint8_t *d, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t n = utf8_decode(d + i, len - i, NULL);
        if (n == 0) return 0;
        i += n;
    }
    return 1;
}

size_t unit_start(const uint8_t *d, size_t len, size_t p)
{
    size_t s;
    if (utf8_inside(d, len, p, &s, NULL)) return s;
    return p;
}

size_t unit_end(const uint8_t *d, size_t len, size_t p)
{
    size_t n = utf8_decode(d + p, len - p, NULL);
    return p + (n ? n : 1);
}
