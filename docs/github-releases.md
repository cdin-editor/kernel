# GitHub Release assets

The Kernel ships installable archives as **GitHub Release assets**. It does not publish
these release packages as OCI images or container-registry artifacts.

## Release trigger

Pushing a version tag matching `v*` starts
[`.github/workflows/release.yml`](../.github/workflows/release.yml). The repository
workflow delegates the cross-platform build, assembly, checksum verification, release
notes, and GitHub Release upload to the reusable
[`m-mdy-m/pipeline` GitHub Release workflow](https://github.com/m-mdy-m/pipeline/blob/main/.github/workflows/release.yml).

The caller explicitly selects Pipeline's `standard` asset profile. The package version
is derived from the tag (for example, `v1.2.3` produces package version `1.2.3`).

## Platform matrix and asset names

| Target | Registry ABI | GitHub Release asset |
| --- | --- | --- |
| Linux x86_64 | `gnu-glibc-2.39` | `cdin-kernel-VERSION-linux-x86_64.tar.gz` |
| Linux ARM64 | `gnu-glibc-2.39` | `cdin-kernel-VERSION-linux-arm64.tar.gz` |
| macOS x86_64 | `macos-15` | `cdin-kernel-VERSION-macos-x86_64.tar.gz` |
| macOS ARM64 | `macos-15` | `cdin-kernel-VERSION-macos-arm64.tar.gz` |
| Windows x86_64 (MinGW-w64 UCRT64) | `windows-ucrt64` | `cdin-kernel-VERSION-windows-x86_64.zip` |

Each archive is accompanied by a `.sha256` sidecar. Pipeline assembles the platform
outputs into one release and adds the aggregate `SHA256SUMS` manifest. GitHub also
provides the tagged source code as zip and tar.gz downloads.

## Registry publication

After all five matrix targets finish, Pipeline downloads their release outputs into
one assembly workspace. The Kernel-owned
`scripts/generate_registry_metadata.py` validates the exact target inventory,
source tag/commit identity, package version, C ABI version, archive structure, target
ABI recorded in each package, and the archive's actual SHA-256/byte count.

It then produces `kernel-VERSION.json` with Registry Component Release Metadata
schema 1.2.0. The generated file is placed under `release-build-metadata/` so Pipeline's
release assembler treats it as a release-build output, flattens it alongside the
five packages, and includes it in the final `SHA256SUMS`. The document records the
exact full source commit, the provided
`org.cdin.kernel-abi` contract, and five target-specific `native-development-package`
artifacts using the `cmake-prefix-v1` layout. GitHub's Release Assets API supplies
the generated metadata asset's own digest and size; the JSON does not attempt to hash
itself.

The metadata asset is generated after the five native build artifacts are assembled.
It is not committed under `registry/releases/`, and an already published metadata
document is never retargeted. Catalog discovery maps the stable `kernel/default`
identity to this repository once; Registry resolves versions from the complete
eligible Release inventory.

## Build and verification contract

Each matrix runner executes `scripts/ci-check.sh` and `scripts/ci-package.sh`.
The package builder calls `scripts/package-release.sh`, which creates exactly one
platform-specific archive and its SHA-256 sidecar. Shared-library aliases are
materialized as regular files before archiving: Registry deliberately rejects
symbolic links and special files in package archives.

Before upload, `scripts/verify-release-output.sh` rejects unexpected files, requires
both expected files, and verifies the archive checksum. The build script then runs
`scripts/test-release-package.sh` against the generated archive to validate its
installer and installed library contents. The release metadata generator performs a
second archive-level check and refuses publication if any target is missing or any
archive contains links or special entries.

The release workflow waits for all five targets, assembles their artifacts, generates
the immutable Registry metadata asset, verifies release checksums, and only then
publishes the GitHub Release.

## Historical release layout

Existing releases such as [v1.2.2](https://github.com/cdin-editor/kernel/releases/tag/v1.2.2)
remain unchanged. New Registry metadata applies only to releases published after this
workflow change; historical release assets and the checked-in
`registry/releases/kernel-1.2.1.json` snapshot remain immutable.
