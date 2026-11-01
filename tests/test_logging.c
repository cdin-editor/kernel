/* Verify the m-mdy-m/log.c adapter, severity mapping, prefix and options. */
#include <cdin/kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_RECORDS 32u
#define LOG_LINE_BYTES 2048u

typedef struct captured_record {
    int level;
    size_t length;
    int nul_terminated;
    char line[LOG_LINE_BYTES];
} captured_record;

typedef struct log_capture {
    size_t calls;
    captured_record records[LOG_RECORDS];
} log_capture;

static void *test_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void *test_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void test_free(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    (void)size;
    free(ptr);
}

static void capture_log(void *ctx, int level, const char *message, size_t length)
{
    log_capture *capture = (log_capture *)ctx;
    captured_record *record;
    size_t copy_length;

    if (capture->calls >= LOG_RECORDS) return;
    record = &capture->records[capture->calls++];
    record->nul_terminated = message[length] == '\0';
    copy_length = length;
    if (copy_length >= sizeof record->line) copy_length = sizeof record->line - 1u;
    memcpy(record->line, message, copy_length);
    record->line[copy_length] = '\0';
    record->length = length;
    record->level = level;
}

static int has_level(const log_capture *capture, int level, const char *message)
{
    size_t i;
    for (i = 0; i < capture->calls; ++i) {
        if (capture->records[i].level == level &&
            (message == NULL || strstr(capture->records[i].line, message) != NULL))
            return 1;
    }
    return 0;
}

static int all_records_have_kernel_prefix(const log_capture *capture)
{
    size_t i;
    static const char prefix[] = "[KERNEL] ";
    for (i = 0; i < capture->calls; ++i) {
        if (capture->records[i].length < sizeof prefix - 1u ||
            !capture->records[i].nul_terminated ||
            strncmp(capture->records[i].line, prefix, sizeof prefix - 1u) != 0)
            return 0;
    }
    return 1;
}

int main(void)
{
    cdin_kernel_config config = {0};
    cdin_log_options log_options;
    cdin_kernel *kernel = NULL;
    cdin_doc doc = CDIN_INVALID_DOC;
    cdin_snapshot snapshot = CDIN_INVALID_SNAPSHOT;
    cdin_version version = 0;
    uint64_t sequence = 0;
    log_capture capture = {0};
    const uint8_t initial[] = "logger adapter";
    cdin_status status;
    size_t before_filter;

    config.struct_size = (uint32_t)sizeof config;
    config.abi_major = CDIN_KERNEL_ABI_MAJOR;
    config.allocator.struct_size = (uint32_t)sizeof config.allocator;
    config.allocator.alloc = test_alloc;
    config.allocator.realloc = test_realloc;
    config.allocator.free = test_free;
    config.log.struct_size = (uint32_t)sizeof config.log;
    config.log.ctx = &capture;
    config.log.log = capture_log;

    status = cdin_kernel_create(&config, &kernel);
    if (status != CDIN_OK) goto fail;
    if (!has_level(&capture, CDIN_LOG_INFO, "kernel initialized")) goto fail;

    status = cdin_log_options_init(&log_options);
    if (status != CDIN_OK) goto fail;
    log_options.min_level = CDIN_LOG_TRACE;
    log_options.output = CDIN_LOG_OUTPUT_CALLBACK;
    log_options.format_flags = CDIN_LOG_F_DEFAULT;
    log_options.max_message_bytes = 1024u;
    status = cdin_kernel_set_log_options(kernel, &log_options);
    if (status != CDIN_OK) goto fail;

    /* Every severity supported by m-mdy-m/log.c is preserved by the adapter. */
    if (cdin_kernel_log(kernel, CDIN_LOG_TRACE, "logging-test", "trace check") != CDIN_OK) goto fail;
    if (cdin_kernel_log(kernel, CDIN_LOG_DEBUG, "logging-test", "debug check") != CDIN_OK) goto fail;
    if (cdin_kernel_log(kernel, CDIN_LOG_INFO, "logging-test", "info check") != CDIN_OK) goto fail;
    if (cdin_kernel_log(kernel, CDIN_LOG_WARN, "logging-test", "warn check") != CDIN_OK) goto fail;
    if (cdin_kernel_log(kernel, CDIN_LOG_ERROR, "logging-test", "error check") != CDIN_OK) goto fail;
    if (cdin_kernel_log(kernel, CDIN_LOG_FATAL, "logging-test", "fatal check") != CDIN_OK) goto fail;

    /* The level setter is live and suppresses messages below its threshold. */
    status = cdin_kernel_set_log_level(kernel, CDIN_LOG_DEBUG);
    if (status != CDIN_OK) goto fail;
    before_filter = capture.calls;
    if (cdin_kernel_log(kernel, CDIN_LOG_TRACE, "logging-test", "filtered trace") != CDIN_OK) goto fail;
    if (capture.calls != before_filter) goto fail;
    if (cdin_kernel_log(kernel, CDIN_LOG_DEBUG, "logging-test", "debug after filter") != CDIN_OK) goto fail;
    if (capture.calls != before_filter + 1u) goto fail;
    status = cdin_kernel_set_log_level(kernel, CDIN_LOG_TRACE);
    if (status != CDIN_OK) goto fail;

    status = cdin_kernel_last_sequence(kernel, &sequence);
    if (status != CDIN_OK || sequence != 0u) goto fail;
    status = cdin_doc_create(kernel, initial, sizeof initial - 1u, &doc);
    if (status != CDIN_OK) goto fail;
    status = cdin_snapshot_acquire(kernel, doc, &version, &snapshot);
    if (status != CDIN_OK) goto fail;
    status = cdin_kernel_shutdown(kernel, CDIN_SHUTDOWN_GRACEFUL);
    if (status != CDIN_OK) goto fail;

    /* Destroying with an outstanding snapshot emits the documented warning. */
    status = cdin_kernel_destroy(kernel);
    kernel = NULL;
    if (status != CDIN_OK) goto fail;

    if (!all_records_have_kernel_prefix(&capture) ||
        !has_level(&capture, CDIN_LOG_DEBUG, "document created") ||
        !has_level(&capture, CDIN_LOG_TRACE, "trace check") ||
        !has_level(&capture, CDIN_LOG_DEBUG, "debug check") ||
        !has_level(&capture, CDIN_LOG_INFO, "info check") ||
        !has_level(&capture, CDIN_LOG_WARN, "warn check") ||
        !has_level(&capture, CDIN_LOG_ERROR, "error check") ||
        !has_level(&capture, CDIN_LOG_FATAL, "fatal check") ||
        !has_level(&capture, CDIN_LOG_WARN, "kernel destroyed with live snapshots") ||
        !has_level(&capture, CDIN_LOG_TRACE, "sequence queried") ||
        !has_level(&capture, CDIN_LOG_INFO, "kernel shutdown started") ||
        !has_level(&capture, CDIN_LOG_DEBUG, "shutdown mode=graceful") ||
        !has_level(&capture, CDIN_LOG_TRACE, "kernel shutdown completed")) {
        fprintf(stderr, "logger adapter output mismatch (%zu records)\n", capture.calls);
        for (size_t i = 0; i < capture.calls; ++i)
            fprintf(stderr, "  level=%d line=%s\n", capture.records[i].level, capture.records[i].line);
        return 1;
    }
    {
        size_t i;
        int found_component = 0;
        for (i = 0; i < capture.calls; ++i) {
            if (strstr(capture.records[i].line, "component=logging-test") != NULL &&
                strstr(capture.records[i].line, "layer=kernel") != NULL) {
                found_component = 1;
                break;
            }
        }
        if (!found_component) {
            fprintf(stderr, "logger metadata missing layer/component\n");
            return 1;
        }
    }
    puts("logging adapter: PASS (prefix, six levels, filtering, metadata, lifecycle)");
    return 0;

fail:
    if (kernel != NULL) (void)cdin_kernel_destroy(kernel);
    fprintf(stderr, "logging adapter setup failed: %s (%u)\n",
            cdin_status_name(status), (unsigned)status);
    return 1;
}
