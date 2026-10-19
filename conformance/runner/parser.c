#include "parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; cdin_status st; } STATUS_NAMES[] = {
    { "CDIN_OK", CDIN_OK },
    { "CDIN_INVALID_ARGUMENT", CDIN_INVALID_ARGUMENT },
    { "CDIN_INVALID_HANDLE", CDIN_INVALID_HANDLE },
    { "CDIN_STALE_VERSION", CDIN_STALE_VERSION },
    { "CDIN_OUT_OF_RANGE", CDIN_OUT_OF_RANGE },
    { "CDIN_INVALID_POSITION", CDIN_INVALID_POSITION },
    { "CDIN_INVALID_STATE", CDIN_INVALID_STATE },
    { "CDIN_BUSY", CDIN_BUSY },
    { "CDIN_LIMIT_EXCEEDED", CDIN_LIMIT_EXCEEDED },
    { "CDIN_NO_MEMORY", CDIN_NO_MEMORY },
    { "CDIN_PATTERN_INVALID", CDIN_PATTERN_INVALID },
    { "CDIN_CANCELLED", CDIN_CANCELLED },
    { "CDIN_SHUTDOWN", CDIN_SHUTDOWN },
    { "CDIN_UNSUPPORTED", CDIN_UNSUPPORTED },
    { "CDIN_BUFFER_TOO_SMALL", CDIN_BUFFER_TOO_SMALL },
    { "CDIN_VERSION_MISMATCH", CDIN_VERSION_MISMATCH },
    { "CDIN_INTERNAL", CDIN_INTERNAL }
};

static const struct { const char *name; int kind; } KIND_NAMES[] = {
    { "edit", CDIN_NOTIF_EDIT },
    { "batch", CDIN_NOTIF_BATCH },
    { "transaction", CDIN_NOTIF_TRANSACTION },
    { "undo", CDIN_NOTIF_UNDO },
    { "redo", CDIN_NOTIF_REDO },
    { "reload", CDIN_NOTIF_RELOAD },
    { "closed", CDIN_NOTIF_CLOSED },
    { "history_evicted", CDIN_NOTIF_HISTORY_EVICTED },
    { "resync", CDIN_NOTIF_RESYNC },
    { "saved_state_changed", CDIN_NOTIF_SAVED_STATE_CHANGED }
};

cdin_status status_from_name(const char *name)
{
    for (size_t i = 0; i < sizeof(STATUS_NAMES) / sizeof(STATUS_NAMES[0]); i++)
        if (strcmp(STATUS_NAMES[i].name, name) == 0) return STATUS_NAMES[i].st;
    return (cdin_status)-1;
}

const char *status_name(cdin_status st)
{
    for (size_t i = 0; i < sizeof(STATUS_NAMES) / sizeof(STATUS_NAMES[0]); i++)
        if (STATUS_NAMES[i].st == st) return STATUS_NAMES[i].name;
    return "CDIN_<unknown>";
}

const char *notif_kind_name(int kind)
{
    for (size_t i = 0; i < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]); i++)
        if (KIND_NAMES[i].kind == kind) return KIND_NAMES[i].name;
    return "<unknown>";
}

static void syntax(vec_ctx *ctx, const char *what, const char *detail)
{
    if (detail)
        fprintf(stderr, "%s:%d: syntax error: %s '%s'\n", ctx->path, ctx->line, what, detail);
    else
        fprintf(stderr, "%s:%d: syntax error: %s\n", ctx->path, ctx->line, what);
    (*ctx->errors)++;
}

int vec_parse_args(const char *p, vec_arg *args, size_t *count, vec_ctx *ctx)
{
    size_t n = 0;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *eq = strchr(p, '=');
        if (!eq) { syntax(ctx, "expected key=value", p); return -1; }
        size_t klen = (size_t)(eq - p);
        if (klen >= sizeof(args[0].key)) { syntax(ctx, "key too long", p); return -1; }
        if (n >= VEC_MAX_ARGS) { syntax(ctx, "too many arguments", p); return -1; }

        char key[32];
        memcpy(key, p, klen);
        key[klen] = '\0';

        const char *v = eq + 1;
        size_t vlen;
        if (*v == '"') {
            v++;
            const char *close = strchr(v, '"');
            if (!close) { syntax(ctx, "unterminated quoted value", key); return -1; }
            vlen = (size_t)(close - v);
            p = close + 1;
            while (*p && !isspace((unsigned char)*p)) { syntax(ctx, "text after quoted value", p); return -1; }
        } else {
            const char *close = v;
            while (*close && !isspace((unsigned char)*close)) close++;
            vlen = (size_t)(close - v);
            p = close;
        }
        if (vlen >= VEC_VAL_MAX) { syntax(ctx, "value too long for", key); return -1; }

        memcpy(args[n].key, key, klen + 1);
        memcpy(args[n].val, v, vlen);
        args[n].val[vlen] = '\0';
        n++;
    }
    *count = n;
    return 0;
}

const char *vec_kv(const vec_arg *args, size_t count, const char *key, vec_ctx *ctx)
{
    const char *found = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(args[i].key, key) != 0) continue;
        if (found) { syntax(ctx, "duplicate key", key); return NULL; }
        found = args[i].val;
    }
    return found;
}

const char *vec_kv_req(const vec_arg *args, size_t count, const char *key, vec_ctx *ctx)
{
    const char *v = vec_kv(args, count, key, ctx);
    if (!v && *ctx->errors == 0) syntax(ctx, "missing required key", key);
    return v;
}

int vec_keys_allowed(const vec_arg *args, size_t count, const char *allowed, vec_ctx *ctx)
{
    int ok = 1;
    for (size_t i = 0; i < count; i++) {
        const char *p = allowed;
        size_t klen = strlen(args[i].key);
        int found = 0;
        while (*p) {
            const char *e = strchr(p, ' ');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l == klen && memcmp(p, args[i].key, l) == 0) { found = 1; break; }
            p = e ? e + 1 : p + l;
        }
        if (!found) { syntax(ctx, "unknown key", args[i].key); ok = 0; }
        if (vec_kv(args, count, args[i].key, ctx) == NULL && *ctx->errors > 0) ok = 0;
    }
    return ok;
}

int vec_u64(const char *v, uint64_t *out)
{
    if (!v || !*v) return -1;
    uint64_t val = 0;
    int base = 10;
    const char *p = v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!*p) return -1;
    for (; *p; p++) {
        unsigned d;
        if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
        else if (base == 16 && *p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a') + 10u;
        else if (base == 16 && *p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A') + 10u;
        else return -1;
        uint64_t next;
        if (val > (UINT64_MAX - d) / (uint64_t)base) return -1;
        next = val * (uint64_t)base + d;
        val = next;
    }
    *out = val;
    return 0;
}

int vec_bytes(const char *v, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t n = 0;
    if (!v) { *out_len = 0; return 0; }
    for (const char *p = v; *p; ) {
        if (n >= cap) return -1;
        if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char hex[3] = { p[2], p[3], '\0' };
            out[n++] = (uint8_t)strtoul(hex, NULL, 16);
            p += 4;
        } else if (p[0] == '\\' && p[1] == '\\') {
            out[n++] = (uint8_t)'\\';
            p += 2;
        } else {
            out[n++] = (uint8_t)*p++;
        }
    }
    *out_len = n;
    return 0;
}
