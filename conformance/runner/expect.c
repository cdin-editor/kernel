/* Expectation checking. Every key an expect line may use is listed in KEYS; a
 * key outside that list is a syntax error, so an expectation can never pass
 * silently. `version` and `count` are op-dependent and resolve to the value the
 * last operation produced (document version, search version, subscription
 * version, match count, event count or line count). */
#include "expect.h"
#include "parser.h"
#include "runner_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *key;
    const uint64_t *slot;
} num_key;

static int check_num(const num_key *table, size_t n, const char *key, const vec_arg *arg,
                     const char *path, int line, int *errors)
{
    uint64_t want = 0;
    if (vec_u64(arg->val, &want) != 0) {
        fprintf(stderr, "%s:%d: syntax error: '%s' is not a number\n", path, line, key);
        (*errors)++;
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].key, key) != 0) continue;
        if (*table[i].slot == want) return 1;
        fprintf(stderr, "%s:%d: %s expected %" PRIu64 ", got %" PRIu64 "\n",
                path, line, key, want, *table[i].slot);
        (*errors)++;
        return 0;
    }
    fprintf(stderr, "%s:%d: syntax error: unknown expectation key '%s'\n", path, line, key);
    (*errors)++;
    return 0;
}

static void check_data(const vec_arg *arg, const char *path, int line, int *errors)
{
    uint8_t want[4096];
    size_t want_len = 0;
    if (vec_bytes(arg->val, want, sizeof(want), &want_len) != 0) {
        fprintf(stderr, "%s:%d: syntax error: data is too long\n", path, line);
        (*errors)++;
        return;
    }
    if (want_len != g_out.data_len || memcmp(want, g_out.data, want_len) != 0) {
        fprintf(stderr, "%s:%d: data mismatch: expected %zu byte(s) ", path, line, want_len);
        for (size_t i = 0; i < want_len; i++) fprintf(stderr, "%02X", want[i]);
        fprintf(stderr, ", got %zu byte(s) ", g_out.data_len);
        for (size_t i = 0; i < g_out.data_len && i < 64; i++) fprintf(stderr, "%02X", g_out.data[i]);
        fputc('\n', stderr);
        (*errors)++;
    }
}

static void check_matches(const vec_arg *arg, const char *path, int line, int *errors)
{
    const char *v = arg->val;
    uint64_t counted = 0;
    uint64_t exp_start[16], exp_end[16];
    while (*v) {
        while (*v == ' ') v++;
        if (!*v) break;
        unsigned long long start = 0, end = 0;
        if (sscanf(v, "%llu:%llu", &start, &end) != 2) {
            fprintf(stderr, "%s:%d: syntax error: matches expects \"start:end\" pairs\n", path, line);
            (*errors)++;
            return;
        }
        if (counted >= 16) {
            fprintf(stderr, "%s:%d: more matches than the runner's range buffer holds\n", path, line);
            (*errors)++;
            return;
        }
        exp_start[counted] = start;
        exp_end[counted] = end;
        counted++;
        while (*v && *v != ' ') v++;
    }
    if (counted != g_out.search.count) {
        fprintf(stderr, "%s:%d: match count expected %" PRIu64 ", got %" PRIu64 "\n",
                path, line, counted, g_out.search.count);
        (*errors)++;
        return;
    }
    for (uint64_t i = 0; i < counted; i++) {
        if (g_search_ranges[i * 2] != exp_start[i] || g_search_ranges[i * 2 + 1] != exp_end[i]) {
            fprintf(stderr, "%s:%d: match %" PRIu64 " expected %" PRIu64 ":%" PRIu64 ", got %" PRIu64 ":%" PRIu64 "\n",
                    path, line, i, exp_start[i], exp_end[i],
                    g_search_ranges[i * 2], g_search_ranges[i * 2 + 1]);
            (*errors)++;
        }
    }
}

static const char *EOL_STYLE_NAMES[] = { "none", "lf", "crlf", "cr", "mixed" };

static void check_eol_style(const vec_arg *arg, const char *path, int line, int *errors)
{
    unsigned got = (unsigned)g_out.num_eol_style;
    const char *name = got < sizeof(EOL_STYLE_NAMES) / sizeof(EOL_STYLE_NAMES[0])
                           ? EOL_STYLE_NAMES[got] : "<unknown>";
    if (strcmp(name, arg->val) == 0) return;
    fprintf(stderr, "%s:%d: style expected %s, got %s\n", path, line, arg->val, name);
    (*errors)++;
}

static void check_kind(const vec_arg *arg, const char *path, int line, int *errors)
{
    const char *name = notif_kind_name((int)g_out.notif_kind);
    if (strcmp(name, arg->val) == 0) return;
    fprintf(stderr, "%s:%d: kind expected %s, got %s\n", path, line, arg->val, name);
    (*errors)++;
}

void expect_check(const char *line, const char *path, int line_num, int *errors)
{
    vec_ctx ctx = { path, line_num, errors };
    vec_arg args[VEC_MAX_ARGS];
    size_t n = 0;

    /* The status is a bare word; the rest are key=value pairs. */
    const char *p = line;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    const char *status_start = p;
    while (*p && *p != ' ') p++;
    const char *status_end = p;
    char status[64];
    size_t len = (size_t)(status_end - status_start);
    if (len == 0 || len >= sizeof(status)) {
        fprintf(stderr, "%s:%d: syntax error: expect needs a status\n", path, line_num);
        (*errors)++;
        return;
    }
    memcpy(status, status_start, len);
    status[len] = '\0';

    if (vec_parse_args(status_end, args, &n, &ctx) != 0) return;

    cdin_status want = status_from_name(status);
    if (want == (cdin_status)-1) {
        fprintf(stderr, "%s:%d: syntax error: unknown status '%s'\n", path, line_num, status);
        (*errors)++;
        return;
    }
    if (g_last_status != want) {
        fprintf(stderr, "%s:%d: status expected %s, got %s\n",
                path, line_num, status, status_name(g_last_status));
        (*errors)++;
    }

    static const num_key KEYS[] = {
        { "major", &g_out.num_abi_major },
        { "minor", &g_out.num_abi_minor },
        { "patch", &g_out.num_abi_patch },
        { "mask", &g_out.num_features },
        { "version", &g_out.num_version },
        { "length", &g_out.num_length },
        { "offset", &g_out.num_offset },
        { "codepoint", &g_out.num_codepoint },
        { "line", &g_out.num_line },
        { "col", &g_out.num_col },
        { "new_version", &g_out.num_new_version },
        { "start", &g_out.num_start },
        { "removed", &g_out.num_removed },
        { "inserted", &g_out.num_inserted },
        { "new_length", &g_out.num_new_length },
        { "count", &g_out.num_count },
        { "current_version", &g_out.num_current_version },
        { "from_version", &g_out.num_from_version },
        { "to_version", &g_out.num_to_version },
        { "next", &g_out.num_next },
        { "prev", &g_out.num_prev },
        { "clean", &g_out.num_clean },
        { "valid", &g_out.num_valid },
        { "truncated", &g_out.num_truncated },
        { "seq", &g_out.num_seq }
    };

    for (size_t i = 0; i < n; i++) {
        const char *key = args[i].key;
        if (strcmp(key, "data") == 0) {
            check_data(&args[i], path, line_num, errors);
        } else if (strcmp(key, "matches") == 0) {
            check_matches(&args[i], path, line_num, errors);
        } else if (strcmp(key, "kind") == 0) {
            check_kind(&args[i], path, line_num, errors);
        } else if (strcmp(key, "style") == 0) {
            check_eol_style(&args[i], path, line_num, errors);
        } else if (strcmp(key, "handle") == 0) {
            if (strcmp(args[i].val, g_last_handle) != 0) {
                fprintf(stderr, "%s:%d: handle expected %s, got %s\n",
                        path, line_num, args[i].val, g_last_handle);
                (*errors)++;
            }
        } else {
            check_num(KEYS, sizeof(KEYS) / sizeof(KEYS[0]), key, &args[i], path, line_num, errors);
        }
    }
}
