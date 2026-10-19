/* Conformance runner: reads vector files, executes their operations and
 * prints one PASS/FAIL line per vector plus a summary. Exit code is non-zero
 * when any vector failed, when any vector could not be read (NOTRUN), or when
 * no vector ran at all. */
#include "expect.h"
#include "ops.h"
#include "parser.h"
#include "runner_state.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    char name[64];
    char path[256];
    int reqs;
    int errors;
    int steps;
    int checks;
    int count;
    int vec_pass;
    int vec_fail;
    int active;
} vector_run;

static void finish(vector_run *run, int quiet, int *v_pass, int *v_fail)
{
    if (!run->active) return;
    if (run->errors > 0) {
        (*v_fail)++;
        if (quiet) return;
        printf("FAIL  %-34s %s  %d failed check(s) of %d\n",
               run->name, run->path, run->errors, run->checks);
    } else {
        (*v_pass)++;
        if (quiet) return;
        printf("PASS  %-34s %s  %d check(s), reqs=%d\n",
               run->name, run->path, run->checks, run->reqs);
    }
}

static char *skip_space(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void trim_eol(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static int run_vector(const char *path, vector_run *run, int quiet)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "NOTRUN %s: cannot open\n", path);
        return -1;
    }
    memset(run, 0, sizeof(*run));
    snprintf(run->path, sizeof(run->path), "%s", path);
    snprintf(run->name, sizeof(run->name), "%s", path);

    char line[4096];
    int line_num = 0;
    int count = 0, v_pass = 0, v_fail = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        trim_eol(line);
        char *p = skip_space(line);
        if (*p == '#' || *p == '\0') continue;

        if (strncmp(p, "vector", 6) == 0 && (p[6] == ' ' || p[6] == '\0')) {
            finish(run, quiet, &v_pass, &v_fail);
            state_reset();
            memset(run, 0, sizeof(*run));
            snprintf(run->path, sizeof(run->path), "%s", path);
            const char *nm = skip_space(p + 6);
            size_t n = 0;
            while (nm[n] && nm[n] != ' ') n++;
            if (n >= sizeof(run->name)) n = sizeof(run->name) - 1;
            memcpy(run->name, nm, n);
            run->name[n] = '\0';
            const char *covers = strstr(p, " covers ");
            if (covers) {
                int ids = 1;
                for (const char *c = covers; *c; c++) if (*c == ' ') ids++;
                run->reqs = ids - 1;
            }
            run->active = 1;
            count++;
            continue;
        }

        if (!run->active) {
            fprintf(stderr, "%s:%d: line before any 'vector' header\n", path, line_num);
            run->errors++;
            continue;
        }

        if (strncmp(p, "expect", 6) == 0 && (p[6] == ' ' || p[6] == '\0')) {
            int before = run->errors;
            expect_check(p, path, line_num, &run->errors);
            run->checks += run->errors == before ? 1 : 0;
            continue;
        }
        run->steps++;
        ops_execute(p, path, line_num, &run->errors);
    }
    fclose(f);

    if (!run->active) {
        fprintf(stderr, "NOTRUN %s: no vector header\n", path);
        return -1;
    }
    finish(run, quiet, &v_pass, &v_fail);
    run->count = count;
    run->vec_pass = v_pass;
    run->vec_fail = v_fail;
    state_reset();        /* destroys every kernel this vector created */
    allocator_report();   /* whatever is still live was never freed */
    return run->errors;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: runner <vec-file> [...]\n");
        return 2;
    }

    int trace = 0;
    int first_file = 1;
    if (strcmp(argv[1], "--trace") == 0) { trace = 1; first_file = 2; }

    int files = 0, vectors = 0, passed = 0, failed = 0, notrun = 0;
    if (trace)
        printf("# vector\tfile\tvectors\tpassed\tfailed\treqs\tchecks\trc\n");
    for (int i = first_file; i < argc; i++) {
        files++;
        vector_run run;
        int rc = run_vector(argv[i], &run, trace);
        vectors += run.count;
        passed += run.vec_pass;
        failed += run.vec_fail;
        if (rc < 0) notrun++;
        if (trace)
            printf("%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n", run.name, run.path, run.count,
                   run.vec_pass, run.vec_fail, run.reqs, run.checks, rc);
    }

    if (files == 0 || vectors == 0) {
        fprintf(stderr, "no vectors ran\n");
        return 2;
    }
    if (trace) return failed ? 1 : 0;

    allocator_gather_violations();
    printf("\n%d file(s), %d vector(s): %d passed, %d failed, %d not run\n",
           files - notrun, vectors, passed, failed, notrun);
    if (g_alloc_violations) printf("%d allocator violation(s)\n", g_alloc_violations);
    return (failed || notrun || g_alloc_violations) ? 1 : 0;
}
