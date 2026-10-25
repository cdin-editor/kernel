# Architecture

This document describes how `cdin-kernel` works inside: the objects, the
contract each module maintains, and the invariants every change must
preserve. The what-from-outside view is `docs/user-guide.md`; the process
for contributors is `docs/development.md`.

## 1. Layering and data flow

```
                 user thread(s)
                      |
             cdin_* public API        (api/*.c: lock + validate + dispatch)
                      |
     +----------------+----------------+
     |                |                |
   doc/txn          text+search      notif
   policy  <----+   implementation   queueing  (notif.c: bounded ring;
   (edit.c      |   (literal.c runs   wake_list.c: deferred callbacks)
   accept/      |    on a PINNED
   prepare)     |    block, no lock)
     |          |
     v          v
   core/ block.c: reference-counted, copy-on-write byte block, with a
   lazily-built line index and trigram filter belonging to the content.
   core/handle_table.c: slot+generation pools (docs/txns/snapshots/subs).
```

Ownership: the `cdin_kernel` owns every allocation directly or through
module structures (docs table, txn table, snapshots, subscriptions, and each
document's history units and text blocks). Nothing escapes to the user
except copies.

## 2. Kernel and lifecycle

`src/kernel.c` owns creation/validation of `cdin_kernel_config`, the lock
entry points (`k_lock`, `k_unlock`), the monotonic `seq` counter, shutdown,
destroy, and the version/feature/status-name queries.

Shutdown semantics live here and nowhere else. A stopped kernel:

- returns `CDIN_SHUTDOWN` from every *mutating* call (`doc_lookup_mut`
  rejects at the entry point of each mutation, subject to the api wrapper
  order),
- aborts every active transaction immediately at shutdown time and frees
  the transaction handles,
- keeps documents, snapshots, subscriptions, and reads alive until
  `cdin_kernel_destroy`.

The convention behind the rule set is deliberate: shutdown completes what
was started and refuses what has not begun. It never closes them for a
caller; the caller decides its own teardown order.

## 3. Documents

`src/doc/doc.c` owns the table (one `pool` for all live documents), plus
create/close and the saved-state and metadata state.

A `doc` struct carries:

- a `cdin_source`-addressable text (`text` pointer),
- metadata (`uint64_t`, no library meaning),
- history state (units, memory counter, clean marker, lost-marker bit,
  `evicted_total`),
- a `tx` pointer while a transaction is active (non-`NULL` blocks other
  mutators with `CDIN_BUSY`),
- subscriber count (`nsubs`) and the document handle used as the event key.

The single resolution helper is `doc_lookup`; its mutating twin
`doc_lookup_mut` additionally rejects a stopped kernel. Reads use the bare
lookup. This distinction is the property that keeps close-and-read working
after shutdown and keeps failed mutations from consuming order slots.

`doc_close_locked` runs under lock: it emits `CLOSED` after the block is
detached but before freeing it (so late subscribers still see valid bytes),
aborts a pending transaction, and detaches subscribers through
`notif.c`'s helper.

## 4. Content and copy-on-write

`src/core/block.c` provides the text block:

- `refs` = number of sharers (document, snapshots, search pins).
- Writes only happen in place while `refs == 1`. Every mutation prepares
  through `text_reserve`, which clones the block when needed. A reader
  therefore never observes bytes changing beneath it.
- The line index and the trigram filter are cached fields of the block and
  invalidated in O(1) on any in-place splice; the clone re-allocates them
  on next use.
- The trigram filter is sized for large documents only and is rebuilt
  lazily; the search path consults it as a *hint*, never as truth.

Text bytes are stored as an opaque byte array. There is no Unicode
normalization stage, no BOM removal, no EOL rewriting, ever. All decoding
happens on demand in `text/` helpers (`utf8.c` is the raw validator).

## 5. Editing

`src/doc/edit.c` is the heart of every content mutation. Every edit
(replace, batch, transaction commit, undo, redo) follows one discipline:

1. **validate**: all structural checks (NULL, sizes, ranges, positions,
   flag sets) against the *current* state, without recording anything.
2. **accept**: version check + sequence number. This step is unfailible
   except for the counter itself; it produces `accepted_seq` and
   `details.expected/current` on failure.
3. **prepare**: every fallible allocation (history record, text space,
   blob copies, notification bookkeeping) happens here. If anything fails,
   everything prepared is freed and the document is untouched; the caller
   sees `CDIN_NO_MEMORY`.
4. **apply**: memmove/memcpy of bytes and linear bookkeeping, none of which
   may fail. Only here is the write visible.

Order note: the buffer-too-small check of an undo/redo blob is part of
validation (step 1) and thus precedes the version check (step 2).
Validation precedence is documented per function in the user guide and
pinned by vectors; do not reorder checks informally.

Batch edits go through the same discipline once: `describe` the incoming
batch, simulate lengths against limits, prepare one history unit containing
all primitive edits, apply from the last range backwards so earlier
offsets stay stable.

Transactions (`src/txn/txn.c`) reuse this machinery: beginning a
transaction takes a sequence number at step 2 (accept), replaces collect
into a tentative block (copy-on-write clone of the doc block, verifying
each attempt against limits), and commit runs the same step-2 version
check + step-3 prepare + step-4 apply in one locked pass. The tentative
block is freed on any failed step of commit, and the transaction handle is
invalidated: a failed commit cannot be retried.

## 6. History

`src/history/undo.c` stores linear units. Each unit has:

- the primitive edits (start, removed bytes, inserted bytes),
- optional before/after caller blobs,
- a coalesce key and, when a batch, an n-uple of edited records.

The clean marker is a unit id: the document is clean exactly when the
newest applied unit is the marker unit, or when no unit was ever applied.
Eviction of the marker unit, or dropping it as a redo tail, sets a
"marker lost" bit and leaves the document dirty until `mark_clean` or
`reload`. Both paths cap memory through `max_undo_memory`: the *memory
accounting* is the invariant, never a unit count.

The `coalesce_key` mechanism exists so that typing bursts form one undo
step. It merges into the preceding unit only when that unit is still the
newest applied one and has the same key and no redo tail. Undo, redo,
mark_clean, reload, and eviction all break the chain. Header
`CDIN_NOTIF_MAX_EDITS` describes the notification cap; the history unit
itself is unbounded in edit count (bounded in memory instead).

## 7. Notifications

`src/notif/notif.c` exposes a ring per subscription. Events are pushed once
per mutation, per subscriber, from the locked region where the mutation
applied - the ring carries the state transition exactly in publication
order.

Two insertion rules make the ring safe under any churn:

1. When the tail is already `RESYNC`, further events only revise its
   `current_version` forward (older vers never roll it back).
2. When the ring is full, everything in it is replaced by a single `RESYNC`
   for the triggering event, so the queue is again guaranteed to fit a
   `RESYNC` plus a `CLOSED`.

`CLOSED` is a special event: it is pushed even into a full ring, and after
it nothing is pushed for that subscription (`doc_closed` flag). Subscribe
and unsubscribe only touch the caller subscription tables; they never call
user code. Wakeup callbacks are queued in `wake_list.c` while the lock is
held and executed - deduplicated per (fn, ctx) pair per mutation - only
after the lock is released.

## 8. Search

`src/search/literal.c` chooses between a fast path and a general scan per
mode:

- **Bytes (case-insensitive off)**: a byte search that may consult the
  block's cached trigram filter to prove absence cheaply for large
  documents. Forward or backward.
- **Literal**: an incremental scanner on valid-UTF-8 code-point boundaries
  with optional case-insensitive simple folding and newline regimes.
  `CDIN_SEARCH_ANY_NEWLINE` turns the LF-or-CRLF comparison into one step
  instead of three.
- **Regex**: defined in the ABI, deliberately unimplemented in this
  release; the dispatcher returns `CDIN_UNSUPPORTED`.

The scan runs with the lock *released* over a pinned, refcounted block
(`SEARCH-010`). The `pin` makes a concurrent mutation clone the block for
the writer (copy-on-write) rather than disturb the scan; results therefore
describe a coherent version. When the scan ends the search re-acquires the
lock solely to publish results, so a stale-document race can still be
detected through `expected_version` at entry (before the pin).

Limits: `step_limit` bounds candidate examinations; `cancel` races on a
relaxed flag whose non-zero turn stops the scan early. Both reduce to
`partial` results when the caller asked for `WANT_PARTIAL`; otherwise only
`details` are written. Nothing else in the kernel consults these fields.

## 9. Text helpers

`src/text/*.c` are pure functions over a pinned or locked block:

- `lines.c` builds the line index lazily and invalidates it on mutation;
  one CRLF is one terminator; a lone CR is one cr not followed by LF; line
  count is `terminators + 1` (so an empty document has one line).
- `positions.c` performs byte<->code-point conversions by walking the text;
  a well-formed sequence is one code point, and each raw byte is its own
  unit. `CDIN_CONV_TOUCHED_RAW` marks having crossed raw bytes.
- `graphemes.c` implements UAX #29 (Unicode data version pinned per
  build by `gen_unicode.py`). A raw byte is always its own cluster; no
  boundary rule may join anything to it.

Correctness note: conversions are `O(offset)` because there is no cached
index for code points; the line index exists precisely because line data
is needed repeatedly and its cost would otherwise be repeated.

## 10. Memory bounds

Every cache is bounded and every allocation is accounted:

| Structure | Bound |
|---|---|
| Documents | `max_documents` |
| Snapshots per doc | `max_snapshots_per_doc` |
| Subscriptions per doc | `max_subscribers_per_doc` (hard 64) |
| Notification ring | `notif_queue_capacity` (4..65536); overflow collapses to RESYNC |
| History units (per doc) | `max_undo_memory` accounting; eviction drops oldest units, never the newest |
| Trigram filter | 2 MiB per text block, only for documents large enough, freed on first mutation touching it |
| Line index | proportional to text size, rebuilt lazily |
| Search index | the trigram filter only, shared with the block; no other persistent cache |

Pool tables double their capacity but are capped by the handle-index space
(slot count `2^24 - 1`), at which point a new object returns
`CDIN_LIMIT_EXCEEDED` and *not* a success with a truncated value. A table
that failed to grow is freed atomically; no handle is lost.

## 11. Invariants every change must preserve

1. Bytes never mutate implicitly: no sanitization, no BOM strip, no EOL
   rewrite on any path that reads, copies, or stores them.
2. The validate -> accept -> prepare -> apply discipline for every mutation.
3. No user callback while the kernel lock is held (allocator is the only
   boundary exception; log, wakeup, and cancel reads never take it).
4. No allocation after the last fallible step of a mutation.
5. Handles cannot be revived: generation bump on release, no reuse of the
   same value for a different object while a stale handle exists. A
   handle outlives its object only as a failed `pool_get`.
6. `k_lock`/`k_unlock` brackets every public path that touches shared
   state; module functions called with or without the lock must state
   which they expect in their comment.
7. Every status other than `CDIN_OK` leaves the observable state as it
   was, modulo the documented sequence consumption and `details` writes.
8. No `_GNU_SOURCE`-style function without a portable fallback; the
   library must compile under `-Werror -Wpedantic` on GCC and clang.
