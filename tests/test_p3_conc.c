/*
 * P3 concurrency tests: real threads against one injected kernel lock.
 *
 * Every test drives several OS threads through the public ABI only, with a
 * platform lock (CRITICAL_SECTION / pthread_mutex) installed as the kernel's
 * lock vtable (ABI-016). The assertions exercise the contract described in docs/architecture.md and docs/user-guide.md:
 * the acceptance point is serialized, sequence numbers are dense and unique,
 * snapshots are immutable, closing and shutting down become visible to other
 * threads as statuses, and two kernels share nothing.
 *
 * The load allocator is the shared harness one, so every test also ends with
 * zero violations and zero live blocks.
 */
#include "cdin/kernel.h"
#include "alloc_strict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   typedef HANDLE th_t;
   typedef CRITICAL_SECTION pt_lock;
#else
#  include <pthread.h>
#  include <sched.h>
   typedef pthread_t th_t;
   typedef pthread_mutex_t pt_lock;
#endif

#define NT   4u    /* worker threads */
#define NIT  200u  /* iterations per worker */

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void banner(const char *name, const char *claim)
{
    printf("%-28s %s\n", name, claim);
}

/* ---- platform threads and locks ---------------------------------------- */

typedef void (*th_fn)(void *);
typedef struct { th_fn fn; void *arg; } th_job;

/* Jobs outlive th_start: the platform takes the pointer by value. */
#define MAX_JOBS 16
static th_job g_jobs[MAX_JOBS];
static int g_job_count;

#ifdef _WIN32
static DWORD WINAPI trampoline_live(LPVOID arg)
{
    th_job *j = (th_job *)arg;
    j->fn(j->arg);
    return 0;
}
#else
static void *trampoline_live(void *arg)
{
    th_job *j = (th_job *)arg;
    j->fn(j->arg);
    return NULL;
}
#endif

static int th_start(th_t *slot, th_fn fn, void *arg)
{
    int i = g_job_count++;
    g_jobs[i].fn = fn;
    g_jobs[i].arg = arg;
#ifdef _WIN32
    *slot = CreateThread(NULL, 0, trampoline_live, &g_jobs[i], 0, NULL);
    return *slot != NULL;
#else
    return pthread_create(slot, NULL, trampoline_live, &g_jobs[i]) == 0;
#endif
}

static void th_join(th_t t)
{
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

static void pt_init(pt_lock *l)
{
#ifdef _WIN32
    InitializeCriticalSection(l);
#else
    pthread_mutex_init(l, NULL);
#endif
}

static void pt_done(pt_lock *l)
{
#ifdef _WIN32
    DeleteCriticalSection(l);
#else
    pthread_mutex_destroy(l);
#endif
}

static void pt_lock_fn(void *v)
{
#ifdef _WIN32
    EnterCriticalSection((pt_lock *)v);
#else
    pthread_mutex_lock((pt_lock *)v);
#endif
}

static void pt_unlock_fn(void *v)
{
#ifdef _WIN32
    LeaveCriticalSection((pt_lock *)v);
#else
    pthread_mutex_unlock((pt_lock *)v);
#endif
}

static void yield_now(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* Start gate: every worker waits here so the calls really do overlap. */
typedef struct {
    int total;
    int arrived;
    int released;
} start_gate;

static void gate_wait(start_gate *g)
{
    if (__atomic_add_fetch(&g->arrived, 1, __ATOMIC_ACQ_REL) == g->total) {
        __atomic_store_n(&g->released, 1, __ATOMIC_RELEASE);
        return;
    }
    while (!__atomic_load_n(&g->released, __ATOMIC_ACQUIRE)) yield_now();
}

static void gate_open(start_gate *g)
{
    g->total = NT;
    g->arrived = g->released = 0;
    g_job_count = 0;
}

/* ---- kernel with a real lock ------------------------------------------- */

typedef struct {
    alloc_ctx *alloc;
    pt_lock lock;
} host;

static cdin_status host_kernel(host *h, cdin_kernel **out,
                               uint64_t max_docs, uint64_t max_doc_size)
{
    cdin_kernel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.abi_major = CDIN_KERNEL_ABI_MAJOR;
    cfg.allocator = alloc_vtable(h->alloc);
    cfg.max_documents = max_docs;
    cfg.max_document_size = max_doc_size;
    cfg.max_undo_memory = 1u << 20;
    cfg.max_blob_size = 4096;
    cfg.max_subscribers_per_doc = 1024;
    cfg.max_snapshots_per_doc = 64;
    cfg.lock.struct_size = sizeof(cfg.lock);
    cfg.lock.ctx = &h->lock;
    cfg.lock.lock = pt_lock_fn;
    cfg.lock.unlock = pt_unlock_fn;
    alloc_set_op(h->alloc, "p3");
    return cdin_kernel_create(&cfg, out);
}

static cdin_status append1(cdin_kernel *k, cdin_doc d, cdin_version expect, cdin_edit_result *er)
{
    memset(er, 0, sizeof(*er));
    er->struct_size = sizeof(*er);
    cdin_replace_request req;
    memset(&req, 0, sizeof(req));
    req.struct_size = sizeof(req);
    req.start = 0;
    req.end = 0;
    req.bytes = (const uint8_t *)"X";
    req.len = 1;
    req.expected_version = expect;
    req.coalesce_key = 0;
    return cdin_doc_replace(k, d, &req, er);
}

/* Every byte must be 'X' and the length must equal the version: each accepted
 * one-byte append adds exactly one of each. */
static int doc_is_clean_append(cdin_kernel *k, cdin_doc d, const char *what)
{
    uint8_t buf[8192];
    uint64_t len = 0, ver = 0;
    size_t got = 0;
    cdin_status st = cdin_text_len(k, cdin_src_doc(d), &len);
    if (st == CDIN_OK) st = cdin_text_version(k, cdin_src_doc(d), &ver);
    if (st != CDIN_OK) { CHECK(0, "%s: len/version %d", what, (int)st); return 0; }
    CHECK(len == ver, "%s: length %llu != version %llu", what,
          (unsigned long long)len, (unsigned long long)ver);
    if (len > sizeof(buf)) { CHECK(0, "%s: %llu bytes", what, (unsigned long long)len); return 0; }
    if (len && cdin_text_read(k, cdin_src_doc(d), 0, len, buf, sizeof(buf), &got, NULL) != CDIN_OK) {
        CHECK(0, "%s: read failed", what);
        return 0;
    }
    for (size_t i = 0; i < got; i++)
        if (buf[i] != 'X') { CHECK(0, "%s: byte %zu is 0x%02X", what, i, buf[i]); return 0; }
    return 1;
}

/* ---- worker bodies ----------------------------------------------------- */

typedef struct {
    cdin_kernel *k;
    cdin_doc d;
    start_gate *gate;
    uint64_t seq[NT * NIT];
    int bad_status;
    cdin_status first_bad;
} worker;

/* CONC-004: the acceptance point is serialized, so concurrent appends to one
 * document produce dense, unique sequence numbers. */
static void w_sequence(void *arg)
{
    worker *w = (worker *)arg;
    cdin_edit_result er;
    gate_wait(w->gate);
    for (unsigned i = 0; i < NIT; i++) {
        cdin_status st = append1(w->k, w->d, CDIN_ANY_VERSION, &er);
        if (st != CDIN_OK) {
            if (!w->bad_status) w->first_bad = st;
            w->bad_status = 1;
            w->seq[i] = 0;
            continue;
        }
        w->seq[i] = er.accepted_seq;
    }
}

/* CONC-003: a stale expected_version is rejected, so exactly one racer wins. */
static void w_race(void *arg)
{
    worker *w = (worker *)arg;
    cdin_edit_result er;
    gate_wait(w->gate);
    w->first_bad = append1(w->k, w->d, 0, &er);
    w->bad_status = (w->first_bad == CDIN_OK) ? 0 : 1;
}

/* Distinct documents: shared kernel state, private content. */
static void w_own_doc(void *arg)
{
    worker *w = (worker *)arg;
    cdin_edit_result er;
    gate_wait(w->gate);
    for (unsigned i = 0; i < NIT; i++) {
        cdin_status st = append1(w->k, w->d, CDIN_ANY_VERSION, &er);
        if (st != CDIN_OK) {
            if (!w->bad_status) w->first_bad = st;
            w->bad_status = 1;
        }
    }
}

/* CONC-005, CONC-006, CONC-011: a snapshot's bytes never change under a
 * writer, so every read returns only bytes that were already written. */
static void w_snapshot_read(void *arg)
{
    worker *w = (worker *)arg;
    uint8_t buf[8192];
    gate_wait(w->gate);
    for (unsigned i = 0; i < NIT; i++) {
        cdin_version v = 0;
        cdin_snapshot s = CDIN_INVALID_SNAPSHOT;
        size_t got = 0;
        if (cdin_snapshot_acquire(w->k, w->d, &v, &s) != CDIN_OK) continue;
        /* The document only ever grows by one 'X' per version. */
        if (v > sizeof(buf)) { w->bad_status = 1; cdin_snapshot_release(w->k, s); break; }
        if (cdin_text_read(w->k, cdin_src_snap(s), 0, v, buf, sizeof(buf), &got, NULL) != CDIN_OK) {
            w->bad_status = 1;
        } else {
            for (size_t b = 0; b < got; b++)
                if (buf[b] != 'X') { w->bad_status = 1; break; }
        }
        if (cdin_snapshot_release(w->k, s) != CDIN_OK) w->bad_status = 1;
    }
}

/* Unsubscribe invalidates the generation-tagged handle. In a race with a
 * writer, only the documented stale-handle status is allowed after removal. */
static void w_subscriber(void *arg)
{
    worker *w = (worker *)arg;
    cdin_notif_event ev[32];
    gate_wait(w->gate);
    for (unsigned i = 0; i < NIT; i++) {
        cdin_notif nf = CDIN_INVALID_NOTIF;
        cdin_version from = 0;
        size_t n = 0;
        cdin_subscribe_request srq;
        memset(&srq, 0, sizeof(srq));
        srq.struct_size = sizeof(srq);
        if (cdin_notif_subscribe(w->k, w->d, &srq, &from, &nf) != CDIN_OK) {
            w->bad_status = 1;
            w->first_bad = CDIN_LIMIT_EXCEEDED;
            continue;
        }
        if (cdin_notif_drain(w->k, nf, ev, 32, &n) != CDIN_OK) w->bad_status = 1;
        if (cdin_notif_unsubscribe(w->k, nf) != CDIN_OK) w->bad_status = 1;
        if (cdin_notif_drain(w->k, nf, ev, 32, &n) != CDIN_INVALID_HANDLE) w->bad_status = 1;
    }
}

/* DOC-close racing reads: only OK or INVALID_HANDLE may be observed. */
static void w_read_until_closed(void *arg)
{
    worker *w = (worker *)arg;
    uint8_t buf[4096];
    gate_wait(w->gate);
    for (unsigned i = 0; i < NIT * 4; i++) {
        uint64_t len = 0;
        cdin_status st = cdin_text_len(w->k, cdin_src_doc(w->d), &len);
        if (st != CDIN_OK && st != CDIN_INVALID_HANDLE) {
            w->bad_status = 1;
            w->first_bad = st;
            break;
        }
        if (st == CDIN_OK) {
            size_t got = 0;
            uint64_t end = len < sizeof(buf) ? len : (uint64_t)sizeof(buf);
            st = cdin_text_read(w->k, cdin_src_doc(w->d), 0, end, buf, sizeof(buf), &got, NULL);
            if (st != CDIN_OK && st != CDIN_INVALID_HANDLE) {
                w->bad_status = 1;
                w->first_bad = st;
                break;
            }
        }
    }
}

/* CONC-020, CONC-021: a shutdown in progress turns new work into BUSY or
 * SHUTDOWN and nothing else. */
static void w_edits_during_shutdown(void *arg)
{
    worker *w = (worker *)arg;
    cdin_edit_result er;
    gate_wait(w->gate);
    for (unsigned i = 0; i < NIT * 4; i++) {
        cdin_status st = append1(w->k, w->d, CDIN_ANY_VERSION, &er);
        if (st == CDIN_OK || st == CDIN_BUSY || st == CDIN_SHUTDOWN) continue;
        w->bad_status = 1;
        w->first_bad = st;
        break;
    }
}

/* ---- harness ----------------------------------------------------------- */

static start_gate g_gate;

static void spawn_all(th_fn fn, worker *w, th_t *slots)
{
    gate_open(&g_gate);
    for (unsigned i = 0; i < NT; i++) w[i].gate = &g_gate;
    for (unsigned i = 0; i < NT; i++)
        CHECK(th_start(&slots[i], fn, &w[i]), "thread %u did not start", i);
}

static void finish(const th_t *slots)
{
    for (unsigned i = 0; i < NT; i++) th_join(slots[i]);
}

static void check_workers(const worker *w, const char *what)
{
    for (unsigned i = 0; i < NT; i++)
        CHECK(!w[i].bad_status, "%s: worker %u saw %d", what, i, (int)w[i].first_bad);
}

static void check_kernel_clean(host *h, const char *what)
{
    if (h->alloc->live_blocks) alloc_report_live(h->alloc, what);
    CHECK(alloc_violations(h->alloc) == 0 && h->alloc->live_blocks == 0,
          "%s: %llu violation(s), %llu leak(s)", what,
          (unsigned long long)alloc_violations(h->alloc),
          (unsigned long long)h->alloc->live_blocks);
}

static void boot(host *h, cdin_kernel **k, const char *name,
                 uint64_t max_docs, uint64_t max_doc_size)
{
    pt_init(&h->lock);
    alloc_init(h->alloc, name, ALLOC_COUNTING | ALLOC_STRICT);
    CHECK(host_kernel(h, k, max_docs, max_doc_size) == CDIN_OK, "create");
}

static void unboot(host *h, cdin_kernel *k, const char *what)
{
    CHECK(cdin_kernel_destroy(k) == CDIN_OK, "destroy");
    check_kernel_clean(h, what);
    pt_done(&h->lock);
}

/* ---- tests ------------------------------------------------------------- */

static void test_sequence_is_dense(void)
{
    banner("test_sequence_is_dense", "CONC-004 sequence numbers are dense and unique");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;
    uint64_t base = 0, seq = 0;
    unsigned char seen[NT * NIT + 1];

    h.alloc = &ctx;
    boot(&h, &k, "p3-seq", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) w[i].k = k;
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    CHECK(cdin_kernel_last_sequence(k, &base) == CDIN_OK, "last_sequence");
    for (unsigned i = 0; i < NT; i++) w[i].d = d;

    spawn_all(w_sequence, w, slots);
    finish(slots);
    check_workers(w, "sequence");

    CHECK(cdin_kernel_last_sequence(k, &seq) == CDIN_OK, "last_sequence");
    CHECK(seq == base + NT * NIT, "last_sequence %llu, expected %llu",
          (unsigned long long)seq, (unsigned long long)(base + NT * NIT));
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < NT; i++) {
        for (unsigned j = 0; j < NIT; j++) {
            uint64_t s = w[i].seq[j];
            CHECK(s >= base + 1 && s <= base + NT * NIT, "seq %llu out of range",
                  (unsigned long long)s);
            if (s >= base + 1 && s <= base + NT * NIT) {
                CHECK(!seen[s - base - 1], "seq %llu used twice", (unsigned long long)s);
                seen[s - base - 1] = 1;
            }
        }
    }
    for (unsigned i = 0; i < NT * NIT; i++) CHECK(seen[i], "seq %u never issued", i + 1);
    doc_is_clean_append(k, d, "seq");
    unboot(&h, k, "seq");
}

static void test_one_winner(void)
{
    banner("test_one_winner", "CONC-003 concurrent expected_version has one winner");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;
    uint64_t len = 0;
    int winners = 0, stale = 0;

    h.alloc = &ctx;
    boot(&h, &k, "p3-race", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) w[i].k = k;
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    for (unsigned i = 0; i < NT; i++) w[i].d = d;

    spawn_all(w_race, w, slots);
    finish(slots);
    for (unsigned i = 0; i < NT; i++) {
        if (w[i].first_bad == CDIN_OK) winners++;
        else if (w[i].first_bad == CDIN_STALE_VERSION) stale++;
        else CHECK(0, "worker %u saw %d", i, (int)w[i].first_bad);
    }
    CHECK(winners == 1, "%d winners, expected 1", winners);
    CHECK(stale == (int)NT - 1, "%d stale, expected %u", stale, NT - 1);
    CHECK(cdin_text_len(k, cdin_src_doc(d), &len) == CDIN_OK && len == 1, "length %llu",
          (unsigned long long)len);
    unboot(&h, k, "race");
}

static void test_shared_kernel(void)
{
    banner("test_shared_kernel", "concurrent documents on one kernel stay separate");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc docs[NT];

    h.alloc = &ctx;
    boot(&h, &k, "p3-docs", 16, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) {
        w[i].k = k;
        CHECK(cdin_doc_create(k, NULL, 0, &docs[i]) == CDIN_OK, "doc_create %u", i);
        w[i].d = docs[i];
    }
    spawn_all(w_own_doc, w, slots);
    finish(slots);
    check_workers(w, "docs");
    for (unsigned i = 0; i < NT; i++) {
        char what[16];
        snprintf(what, sizeof(what), "doc %u", i);
        doc_is_clean_append(k, docs[i], what);
        CHECK(cdin_doc_close(k, docs[i]) == CDIN_OK, "close %u", i);
    }
    unboot(&h, k, "docs");
}

static void test_snapshot_held_across_edits(void)
{
    banner("test_snapshot_held", "CONC-011 a held snapshot never changes");
    alloc_ctx ctx;
    host h;
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;
    cdin_version v = 0;
    cdin_snapshot s = CDIN_INVALID_SNAPSHOT;
    uint8_t before[8192], after[8192];
    size_t n1 = 0, n2 = 0;
    cdin_edit_result er;

    h.alloc = &ctx;
    boot(&h, &k, "p3-snap", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    CHECK(cdin_snapshot_acquire(k, d, &v, &s) == CDIN_OK, "acquire");
    CHECK(cdin_text_read(k, cdin_src_snap(s), 0, v, before, sizeof(before), &n1, NULL) == CDIN_OK, "read");
    for (unsigned i = 0; i < 20; i++)
        CHECK(append1(k, d, CDIN_ANY_VERSION, &er) == CDIN_OK, "append");
    CHECK(cdin_text_read(k, cdin_src_snap(s), 0, v, after, sizeof(after), &n2, NULL) == CDIN_OK, "reread");
    CHECK(n1 == n2 && memcmp(before, after, n1) == 0, "snapshot bytes changed");
    CHECK(cdin_snapshot_release(k, s) == CDIN_OK, "release");
    doc_is_clean_append(k, d, "snap");
    unboot(&h, k, "snap");
}

static void test_snapshot_readers_race_writers(void)
{
    banner("test_snapshot_race", "readers and writers overlap on one document");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;

    h.alloc = &ctx;
    boot(&h, &k, "p3-snaprace", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) w[i].k = k;
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    for (unsigned i = 0; i < NT; i++) w[i].d = d;

    /* Slot 0 writes, slots 1..NT-1 hold and re-read snapshots. */
    gate_open(&g_gate);
    for (unsigned i = 0; i < NT; i++) w[i].gate = &g_gate;
    CHECK(th_start(&slots[0], w_sequence, &w[0]), "writer did not start");
    for (unsigned i = 1; i < NT; i++)
        CHECK(th_start(&slots[i], w_snapshot_read, &w[i]), "reader %u did not start", i);
    finish(slots);
    check_workers(w, "snaprace");
    doc_is_clean_append(k, d, "snaprace");
    unboot(&h, k, "snaprace");
}

static void test_subscriber_lifecycle(void)
{
    banner("test_subscriber", "subscribe/drain/unsubscribe during edits");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;

    h.alloc = &ctx;
    boot(&h, &k, "p3-notif", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) w[i].k = k;
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    for (unsigned i = 0; i < NT; i++) w[i].d = d;

    gate_open(&g_gate);
    for (unsigned i = 0; i < NT; i++) w[i].gate = &g_gate;
    CHECK(th_start(&slots[0], w_sequence, &w[0]), "writer did not start");
    for (unsigned i = 1; i < NT; i++)
        CHECK(th_start(&slots[i], w_subscriber, &w[i]), "churn %u did not start", i);
    finish(slots);
    check_workers(w, "notif");
    unboot(&h, k, "notif");
}

static void test_close_races_reads(void)
{
    banner("test_close_races_reads", "a closed document reads as INVALID_HANDLE");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;
    uint64_t len = 0;

    h.alloc = &ctx;
    boot(&h, &k, "p3-close", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) w[i].k = k;
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    for (unsigned i = 0; i < NT; i++) w[i].d = d;

    gate_open(&g_gate);
    for (unsigned i = 0; i < NT; i++) w[i].gate = &g_gate;
    for (unsigned i = 0; i < NT - 1; i++)
        CHECK(th_start(&slots[i], w_read_until_closed, &w[i]), "reader %u did not start", i);
    CHECK(th_start(&slots[NT - 1], w_own_doc, &w[NT - 1]), "writer did not start");
    finish(slots);
    check_workers(w, "close");
    CHECK(cdin_doc_close(k, d) == CDIN_OK, "close");
    CHECK(cdin_text_len(k, cdin_src_doc(d), &len) == CDIN_INVALID_HANDLE, "closed doc is still readable");
    unboot(&h, k, "close");
}

static void test_shutdown_during_edits(void)
{
    banner("test_shutdown_during_edits", "CONC-020 in-flight work finishes or is refused");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc fresh = CDIN_INVALID_DOC;
    int spin;

    h.alloc = &ctx;
    boot(&h, &k, "p3-shut", 16, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) {
        w[i].k = k;
        CHECK(cdin_doc_create(k, NULL, 0, &w[i].d) == CDIN_OK, "doc_create %u", i);
    }
    gate_open(&g_gate);
    for (unsigned i = 0; i < NT; i++) w[i].gate = &g_gate;
    for (unsigned i = 0; i < NT; i++)
        CHECK(th_start(&slots[i], w_edits_during_shutdown, &w[i]), "thread %u did not start", i);
    /* Shut down while they run: the gate is open, so they are mid-flight. */
    for (spin = 0; spin < 50000; spin++) yield_now();
    CHECK(cdin_kernel_shutdown(k, CDIN_SHUTDOWN_GRACEFUL) == CDIN_OK, "shutdown");
    finish(slots);
    check_workers(w, "shutdown");
    CHECK(cdin_kernel_shutdown(k, CDIN_SHUTDOWN_GRACEFUL) == CDIN_OK, "repeated shutdown");
    CHECK(cdin_doc_create(k, NULL, 0, &fresh) == CDIN_SHUTDOWN, "create after shutdown");
    unboot(&h, k, "shutdown");
}

/* ABI-012: two kernels share nothing, including their shutdown state. */
static void test_two_kernels(void)
{
    banner("test_two_kernels", "two live kernels never touch each other");
    alloc_ctx ctx1, ctx2;
    host h1, h2;
    worker w[NT];
    th_t s1[NT / 2], s2[NT / 2];
    cdin_kernel *k1 = NULL, *k2 = NULL;
    cdin_doc d1 = CDIN_INVALID_DOC, d2 = CDIN_INVALID_DOC, fresh = CDIN_INVALID_DOC;
    cdin_edit_result er;

    h1.alloc = &ctx1;
    h2.alloc = &ctx2;
    boot(&h1, &k1, "p3-k1", 8, 65536);
    boot(&h2, &k2, "p3-k2", 8, 65536);
    if (!k1 || !k2) { pt_done(&h1.lock); pt_done(&h2.lock); return; }
    memset(w, 0, sizeof(w));
    CHECK(cdin_doc_create(k1, NULL, 0, &d1) == CDIN_OK, "doc k1");
    CHECK(cdin_doc_create(k2, NULL, 0, &d2) == CDIN_OK, "doc k2");
    for (unsigned i = 0; i < NT / 2; i++) {
        w[i].k = k1;
        w[i].d = d1;
        w[NT / 2 + i].k = k2;
        w[NT / 2 + i].d = d2;
    }

    gate_open(&g_gate);
    for (unsigned i = 0; i < NT; i++) w[i].gate = &g_gate;
    for (unsigned i = 0; i < NT / 2; i++)
        CHECK(th_start(&s1[i], w_own_doc, &w[i]), "k1 thread %u", i);
    for (unsigned i = 0; i < NT / 2; i++)
        CHECK(th_start(&s2[i], w_own_doc, &w[NT / 2 + i]), "k2 thread %u", i);
    for (unsigned i = 0; i < NT / 2; i++) th_join(s1[i]);
    for (unsigned i = 0; i < NT / 2; i++) th_join(s2[i]);
    check_workers(w, "isolation");
    doc_is_clean_append(k1, d1, "k1");
    doc_is_clean_append(k2, d2, "k2");

    /* Shutting one down leaves the other working. */
    CHECK(cdin_kernel_shutdown(k1, CDIN_SHUTDOWN_GRACEFUL) == CDIN_OK, "shutdown k1");
    CHECK(append1(k2, d2, CDIN_ANY_VERSION, &er) == CDIN_OK, "k2 edits after k1 shutdown");
    CHECK(cdin_doc_create(k2, NULL, 0, &fresh) == CDIN_OK, "k2 create after k1 shutdown");
    CHECK(cdin_doc_close(k2, fresh) == CDIN_OK, "k2 close");

    unboot(&h1, k1, "k1");
    unboot(&h2, k2, "k2");
}

/* ERR-005 under load: an injected failure never corrupts the document, and the
 * length still equals the version because both only advance on acceptance. */
static void test_alloc_failure_under_load(void)
{
    banner("test_alloc_failure_load", "ERR-005 nothing partial survives OOM");
    alloc_ctx ctx;
    host h;
    worker w[NT];
    th_t slots[NT];
    cdin_kernel *k = NULL;
    cdin_doc d = CDIN_INVALID_DOC;

    h.alloc = &ctx;
    boot(&h, &k, "p3-oom", 8, 65536);
    if (!k) { pt_done(&h.lock); return; }
    memset(w, 0, sizeof(w));
    for (unsigned i = 0; i < NT; i++) w[i].k = k;
    CHECK(cdin_doc_create(k, NULL, 0, &d) == CDIN_OK, "doc_create");
    for (unsigned i = 0; i < NT; i++) w[i].d = d;
    /* Let some edits through, then refuse every further allocation. */
    ctx.fail_after = 40;

    spawn_all(w_sequence, w, slots);
    finish(slots);
    ctx.fail_after = UINT64_MAX;
    for (unsigned i = 0; i < NT; i++)
        CHECK(w[i].first_bad == CDIN_OK || w[i].first_bad == CDIN_NO_MEMORY,
              "worker %u saw %d", i, (int)w[i].first_bad);
    doc_is_clean_append(k, d, "oom");
    unboot(&h, k, "oom");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_sequence_is_dense();
    test_one_winner();
    test_shared_kernel();
    test_snapshot_held_across_edits();
    test_snapshot_readers_race_writers();
    test_subscriber_lifecycle();
    test_close_races_reads();
    test_shutdown_during_edits();
    test_two_kernels();
    test_alloc_failure_under_load();
    if (failures) {
        printf("%d P3 check(s) failed\n", failures);
        return 1;
    }
    printf("all P3 concurrency checks passed\n");
    return 0;
}
