# Documentation

This site/document set describes the behavior implemented in this repository. When prose and code disagree, `include/cdin/kernel.h` and the corresponding implementation are authoritative; please report discrepancies rather than relying on an old example.

## Read in this order

1. **[User guide](user-guide.md)** — the intended first stop for anyone integrating the engine.
2. [Getting started](getting-started.md) — requirements, build commands, static/shared linking, and a first run.
3. [Installation and packaging](installation.md) — stage libraries, runtime lookup, and release compatibility.
4. [ABI reference](abi.md) — status values, handles, request/output structs, memory callbacks, and every public function family.
5. [Language bindings](language-bindings.md) — C, Python `ctypes`, Go `cgo`, JavaScript and TypeScript.
6. [Limits and status codes](limits-and-status.md) — defaults, unsupported features, errors, and safe fallbacks.
7. [Architecture](architecture.md) — ownership, modules, versions, snapshots, locking, and notifications.
8. [Developer guide](development.md) — tests, conformance vectors, sanitizers, fuzzing, and contribution workflow.

## Contract language

- **Guaranteed by the current public contract** means documented in the public header and implemented in the current sources.
- **Current limitation** means callers should not expect the feature to work just because a type, enum, or reserved flag exists.
- **Example bridge** means code in `examples/ffi/` intended to teach integration; it is not a second official ABI and must be rebuilt together with the core.

Project status is stable for C ABI `1.0.0`, synchronized with CDIN Spec `1.0.0`. Review the compatibility section in [ABI reference](abi.md) and use the exact shipped header before releasing bindings.
