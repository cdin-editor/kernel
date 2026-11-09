# GitHub Release assets

The kernel ships installable archives as **GitHub Release assets**. It does not publish
these release packages as OCI images or container-registry artifacts.

## Release trigger

Pushing a version tag matching `v*` starts
[`.github/workflows/release.yml`](../.github/workflows/release.yml). The repository
workflow delegates the cross-platform build, assembly, checksum verification, release
notes, and GitHub Release upload to the reusable
[`m-mdy-m/pipeline` GitHub Release workflow](https://github.com/m-mdy-m/pipeline/blob/main/.github/workflows/release.yml).

The caller explicitly selects Pipeline's `standard` asset profile. The package version
is derived from the tag (for example, `v1.2.1` produces package version `1.2.1`).

## Platform matrix and asset names

| Target | GitHub Release asset |
| --- | --- |
| Linux x86_64 | `cdin-kernel-VERSION-linux-x86_64.tar.gz` |
| Linux ARM64 | `cdin-kernel-VERSION-linux-arm64.tar.gz` |
| macOS x86_64 | `cdin-kernel-VERSION-macos-x86_64.tar.gz` |
| macOS ARM64 | `cdin-kernel-VERSION-macos-arm64.tar.gz` |
| Windows x86_64 (MinGW-w64 UCRT64) | `cdin-kernel-VERSION-windows-x86_64.zip` |

Each archive is accompanied by a `.sha256` sidecar. Pipeline assembles the platform
outputs into one release and adds the aggregate `SHA256SUMS` manifest. GitHub also
provides the tagged source code as zip and tar.gz downloads.

## Build and verification contract

Each matrix runner executes `scripts/ci-check.sh` and `scripts/ci-package.sh`.
The package builder calls `scripts/package-release.sh`, which creates exactly one
platform-specific archive and its SHA-256 sidecar. Before upload,
`scripts/verify-release-output.sh` rejects unexpected files, requires both expected
files, and verifies the archive checksum. The build script then runs
`scripts/test-release-package.sh` against the generated archive to validate its
installer and installed library contents.

The release workflow waits for all five matrix targets, assembles their artifacts,
verifies the checksums, and only then publishes the GitHub Release assets.

## Reference release

The existing [v1.2.1 release](https://github.com/cdin-editor/kernel/releases/tag/v1.2.1)
is the reference layout: five platform archives, five matching `.sha256` files,
`SHA256SUMS`, and GitHub's source-code zip/tar.gz downloads.
