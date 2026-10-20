/**
 * @file quickstart.c
 * @brief Minimal example using the installed cdin-kernel C API.
 *
 * From the repository root, run `make quickstart`. For an installed library,
 * compile this file with the include and linker flags from `pkg-config`.
 */
#include <cdin/kernel.h>

#include <stdio.h>
#include <stdlib.h>

/* The kernel receives its allocator from the application. These callbacks
 * adapt the standard C allocator to the size-aware cdin_allocator contract. */
static void *default_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void *default_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size; /* realloc() does not need the previous size. */
    return realloc(ptr, new_size);
}

static void default_free(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    (void)size; /* free() does not need the allocation size. */
    free(ptr);
}

/* Keep each API call checked, and send every failure through shared cleanup. */
#define CHECK(call) do { \
    cdin_status check_status = (call); \
    if (check_status != CDIN_OK) { \
        fprintf(stderr, "%s failed: %s (%u)\n", #call, \
                cdin_status_name(check_status), (unsigned)check_status); \
        goto cleanup; \
    } \
} while (0)

int main(void)
{
    /* Prepare the configuration and install the mandatory allocator. */
    cdin_kernel_config config = {0};
    cdin_kernel *kernel = NULL;
    cdin_doc doc = CDIN_INVALID_DOC;
    const uint8_t initial_text[] = "Hello world\n";
    const uint8_t replacement[] = "CDIN";
    cdin_replace_request replace = {0};
    cdin_edit_result edit = {0};
    uint64_t text_length = 0;
    size_t bytes_read = 0;
    uint8_t *text = NULL;
    int exit_code = 1;

    config.struct_size = (uint32_t)sizeof(config);
    config.abi_major = CDIN_KERNEL_ABI_MAJOR;
    config.allocator.struct_size = (uint32_t)sizeof(config.allocator);
    config.allocator.alloc = default_alloc;
    config.allocator.realloc = default_realloc;
    config.allocator.free = default_free;
    config.lock.struct_size = (uint32_t)sizeof(config.lock);
    config.log.struct_size = (uint32_t)sizeof(config.log);

    /* Create a kernel and copy the initial bytes into a new document. */
    CHECK(cdin_kernel_create(&config, &kernel));
    CHECK(cdin_doc_create(kernel, initial_text, sizeof(initial_text) - 1, &doc));

    /* Replace the byte range [6, 11), which contains the word "world". */
    replace.struct_size = (uint32_t)sizeof(replace);
    replace.expected_version = 0; /* Every new document starts at version 0. */
    replace.start = 6;
    replace.end = 11;
    replace.bytes = replacement;
    replace.len = sizeof(replacement) - 1;
    edit.struct_size = (uint32_t)sizeof(edit);
    CHECK(cdin_doc_replace(kernel, doc, &replace, &edit));

    /* Query the text size, allocate caller-owned memory, then copy the bytes. */
    CHECK(cdin_text_len(kernel, cdin_src_doc(doc), &text_length));
    if (text_length > (uint64_t)SIZE_MAX) {
        fputs("Document is too large for this process.\n", stderr);
        goto cleanup;
    }

    text = (uint8_t *)malloc(text_length ? (size_t)text_length : 1u);
    if (text == NULL) {
        fputs("Out of memory.\n", stderr);
        goto cleanup;
    }

    CHECK(cdin_text_read(kernel, cdin_src_doc(doc), 0, text_length,
                         text, (size_t)text_length, &bytes_read, NULL));
    if (fwrite(text, 1, bytes_read, stdout) != bytes_read) {
        fputs("Failed to write document bytes to stdout.\n", stderr);
        goto cleanup;
    }

    fprintf(stderr, "document version: %llu\n",
            (unsigned long long)edit.new_version);
    exit_code = 0;

cleanup:
    /* Close owned objects before destroying the kernel; both are safe to skip
     * when setup failed before the corresponding handle was created. */
    free(text);
    if (kernel != NULL && doc != CDIN_INVALID_DOC) {
        cdin_status status = cdin_doc_close(kernel, doc);
        if (status != CDIN_OK && exit_code == 0) exit_code = 1;
    }
    if (kernel != NULL) {
        cdin_status status = cdin_kernel_destroy(kernel);
        if (status != CDIN_OK && exit_code == 0) exit_code = 1;
    }
    return exit_code;
}
