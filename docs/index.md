# Documentation

Use these guides in order when integrating `cdin-kernel` into an application.

1. **[Getting started](getting-started.md)** — build, install, include, and link the library.
2. **[User guide](user-guide.md)** — documents, edits, versions, undo/redo, transactions, snapshots, search, notifications, and positions.
3. **[Installation and packaging](installation.md)** — platform packages, custom prefixes, `pkg-config`, and the Docker SDK image.
4. **[C ABI reference](abi.md)** — public types, status values, structures, and function families.
5. **[Language bindings](language-bindings.md)** — Python, Go, JavaScript, and TypeScript examples.
6. **[Limits and status codes](limits-and-status.md)** — configured limits, errors, and unsupported features.
7. **[Architecture](architecture.md)** — internal modules, ownership, versions, locking, and notifications.
8. **[Developer guide](development.md)** — tests, conformance vectors, sanitizers, fuzzing, and contribution workflow.
9. **[Conformance-vector format](../conformance/FORMAT.md)** — fixture syntax and runner behavior.

## Compatibility

The current public C ABI is `1.0.0`. Compile bindings against the header shipped with the same release as the library, and call `cdin_abi_version()` during initialization when your application loads the library dynamically. The implementation and `include/cdin/kernel.h` are the authoritative source for current behavior; report any difference found in these guides.
