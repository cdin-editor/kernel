/*
 * Fuzz target: vector parser.
 * Feed random bytes as vector file content.
 * Run under ASan/UBSan.
 */
#include "cdin/kernel.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Minimal vector parser fuzz — exercises tokenization paths */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Just verify no crash on arbitrary input through the vector format */
    /* The actual parser is in conformance/runner.c; this is a placeholder */
    /* for a future standalone parser fuzz target */
    (void)data;
    (void)size;
    return 0;
}
