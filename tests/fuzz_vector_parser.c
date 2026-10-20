/* Fuzz the real conformance-vector argument parser and typed accessors. */
#include "parser.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *input;
    size_t n = size < 4095 ? size : 4095;
    vec_arg args[VEC_MAX_ARGS];
    size_t count = 0;
    int errors = 0;
    vec_ctx ctx = { NULL, 0, &errors };
    uint8_t bytes[VEC_VAL_MAX];

    input = (char *)malloc(n + 1);
    if (!input) return 0;
    if (n) memcpy(input, data, n);
    input[n] = '\0';

    if (vec_parse_args(input, args, &count, &ctx) == 0) {
        (void)vec_keys_allowed(args, count,
            "start end expected_version bytes len flags mode direction count handle", &ctx);
        for (size_t i = 0; i < count; i++) {
            uint64_t number = 0;
            size_t bytes_len = 0;
            (void)vec_u64(args[i].val, &number);
            (void)vec_bytes(args[i].val, bytes, sizeof bytes, &bytes_len);
            (void)status_from_name(args[i].val);
            (void)notif_kind_name((int)(number & 0x7fffffffU));
        }
    }
    free(input);
    return 0;
}
