/* Fuzz target: public API inputs. Document bytes and request coordinates are
 * arbitrary; all requests use the stable public ABI (not private internals).
 * Run under libFuzzer + ASan/UBSan. */
#include "cdin/kernel.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *fuzz_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void *fuzz_realloc(void *ctx, void *p, size_t old_n, size_t new_n) {
    (void)ctx; (void)old_n;
    if (new_n == 0) { free(p); return NULL; }
    return realloc(p, new_n);
}
static void fuzz_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16) return 0;
    cdin_kernel_config cfg;
    cdin_allocator alloc;
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;
    size_t doc_len = size > 256 ? 256 : size;
    memset(&cfg, 0, sizeof cfg);
    memset(&alloc, 0, sizeof alloc);
    alloc.struct_size = (uint32_t)sizeof alloc;
    alloc.alloc = fuzz_alloc; alloc.realloc = fuzz_realloc; alloc.free = fuzz_free;
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator = alloc;
    if (cdin_kernel_create(&cfg, &k) != CDIN_OK) return 0;
    if (cdin_doc_create(k, data, doc_len, &d) != CDIN_OK) {
        cdin_kernel_destroy(k);
        return 0;
    }

    for (size_t i = 0; i < 8 && i + 4 < size; i++) {
        cdin_status st;
        switch (data[i] % 7) {
        case 0: {
            cdin_version v = 0;
            st = cdin_text_version(k, cdin_src_doc(d), &v);
            (void)st;
            break;
        }
        case 1: {
            uint64_t len = 0;
            st = cdin_text_len(k, cdin_src_doc(d), &len);
            (void)st;
            break;
        }
        case 2: {
            cdin_replace_request req;
            cdin_edit_result out;
            memset(&req, 0, sizeof req); memset(&out, 0, sizeof out);
            req.struct_size = (uint32_t)sizeof req;
            req.start = data[i + 1] % (doc_len + 1);
            req.end = req.start;
            req.bytes = data + i + 2; req.len = 1;
            req.expected_version = CDIN_ANY_VERSION;
            out.struct_size = (uint32_t)sizeof out;
            st = cdin_doc_replace(k, d, &req, &out);
            (void)st;
            break;
        }
        case 3:
        case 4: {
            cdin_history_request req;
            cdin_edit_result out;
            memset(&req, 0, sizeof req); memset(&out, 0, sizeof out);
            req.struct_size = (uint32_t)sizeof req;
            req.expected_version = CDIN_ANY_VERSION;
            out.struct_size = (uint32_t)sizeof out;
            st = data[i] % 7 == 3 ? cdin_doc_undo(k, d, &req, &out)
                                  : cdin_doc_redo(k, d, &req, &out);
            (void)st;
            break;
        }
        case 5: {
            uint64_t len = 0;
            st = cdin_text_len(k, cdin_src_doc(d), &len);
            if (st == CDIN_OK) {
                uint64_t off = len ? data[i + 1] % (len + 1) : 0;
                (void)cdin_text_check_position(k, cdin_src_doc(d), CDIN_ANY_VERSION, off);
            }
            break;
        }
        default:
            (void)cdin_doc_mark_clean(k, d);
            break;
        }
    }
    (void)cdin_doc_close(k, d);
    (void)cdin_kernel_destroy(k);
    return 0;
}
