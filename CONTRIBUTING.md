# Contributing

`cdin-kernel` is a C11 text engine with a C ABI. The authoritative API declarations are in [`include/cdin/kernel.h`](include/cdin/kernel.h); documentation is organized under [`docs/`](docs/index.md).

## Before you start

Read the [user guide](docs/user-guide.md) to understand behavioral contracts, the [ABI reference](docs/abi.md) before changing public declarations, and the [developer guide](docs/development.md) for build/test steps. Read [`conformance/FORMAT.md`](conformance/FORMAT.md) before adding a vector.

## Making a change

1. Branch from the project development branch (`feature/...` or `bugfix/...`; check repository workflow configuration if unsure).
2. For externally observable behavior, add or update conformance vectors before changing implementation where practical.
3. Keep the change focused and build after the smallest meaningful step.
4. Run `make check`; run `make asan` when your toolchain supports the sanitizer runtime.
5. Update the relevant user, ABI, support-matrix, or developer documentation. Do not create a requirement-status page by hand; conformance vectors and source tests are the behavioral evidence.
6. Commit using `<type>(<scope>): <subject>` (for example `feat(search): ...`, `fix(txn): ...`, `docs(abi): ...`, `build(make): ...`).

## Rules

- Public ABI changes require project-owner review. The ABI is draft `0.2.0`; preserve compatibility deliberately and update versioning notes when declarations change.
- Every public function reports a `cdin_status`; do not silently discard errors.
- Avoid global mutable state and reentrant calls from callbacks unless the public contract explicitly permits them.
- Do not call `exit`, `abort`, or `assert` on caller-controlled input.
- Route kernel-owned allocation through the injected allocator.
- Preserve document bytes exactly: no BOM stripping, Unicode normalization, or silent UTF-8 replacement.
- Keep conformance vectors and regression tests passing before submitting changes.
- Distinguish implemented behavior from reserved flags/features in documentation.
