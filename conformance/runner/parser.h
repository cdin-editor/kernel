/* Vector line tokenizer: splits "<op> key=value key=\"quoted value\"" into
 * key/value pairs. One parser for ops and expectations; unknown or duplicate
 * keys are rejected by the per-op schema check (ops.c), not here. */
#ifndef PARSER_H
#define PARSER_H

#include "cdin/kernel.h"
#include <stddef.h>
#include <stdint.h>

#define VEC_MAX_ARGS 16
#define VEC_VAL_MAX 1024

typedef struct {
    char key[32];
    char val[VEC_VAL_MAX];
} vec_arg;

typedef struct {
    const char *path;
    int line;
    int *errors;
} vec_ctx;

/* Parses the argument list after the operation name. Returns 0 on success,
 * -1 on syntax error (already reported through ctx->errors). */
int vec_parse_args(const char *p, vec_arg *args, size_t *count, vec_ctx *ctx);

/* Returns the value of `key`, or NULL when absent. Duplicate keys are a syntax
 * error (reported) and return NULL. */
const char *vec_kv(const vec_arg *args, size_t count, const char *key, vec_ctx *ctx);

/* Same, but fails when the key is missing. */
const char *vec_kv_req(const vec_arg *args, size_t count, const char *key, vec_ctx *ctx);

/* Rejects any key not listed in the space-separated `allowed` list. */
int vec_keys_allowed(const vec_arg *args, size_t count, const char *allowed, vec_ctx *ctx);

/* Typed accessors. Return 0 on success, -1 on a missing or malformed value. */
int vec_u64(const char *v, uint64_t *out);
int vec_bytes(const char *v, uint8_t *out, size_t cap, size_t *out_len);

cdin_status status_from_name(const char *name);
const char *status_name(cdin_status st);
const char *notif_kind_name(int kind);

#endif
