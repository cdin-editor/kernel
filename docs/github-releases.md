# GitHub Release assets

The Kernel ships installable archives as **GitHub Release assets**. It does not publish
these release packages as OCI images or container-registry artifacts.

## Release trigger

Pushing a version tag matching \`v*\` starts
[\`.github/workflows/release.yml\`](../.github/workflows/release.yml). The repository
workflow delegates the cross-platform build, assembly, checksum verification, release
notes, and GitHub Release upload to the reusable
[\`m-mdy-m/pipeline\` GitHub Release workflow](https://github.com/m-mdy-m/pipeline/blob/main/.github/workflows/release.yml).

The caller explicitly selects Pipeline's \`standard\` asset profile. The package version
is derived from the tag (for example, \`v1.2.3\` produces package version \`1.2.3\`).

## Platform matrix and asset names

| Target | Existing user-download archive | Flat Registry package |
| --- | --- | --- |
| Linux x86_64 | \`cdin-kernel-VERSION-linux-x86_64.tar.gz\` | \`cdin-kernel-VERSION-linux-x86_64-cmake-prefix.tar.gz\` |
| Linux ARM64 | \`cdin-kernel-VERSION-linux-arm64.tar.gz\` | \`cdin-kernel-VERSION-linux-arm64-cmake-prefix.tar.gz\` |
| macOS x86_64 | \`cdin-kernel-VERSION-macos-x86_64.tar.gz\` | \`cdin-kernel-VERSION-macos-x86_64-cmake-prefix.tar.gz\` |
| macOS ARM64 | \`cdin-kernel-VERSION-macos-arm64.tar.gz\` | \`cdin-kernel-VERSION-macos-arm64-cmake-prefix.tar.gz\` |
| Windows x86_64 (MinGW-w64 UCRT64) | \`cdin-kernel-VERSION-windows-x86_64.zip\` | \`cdin-kernel-VERSION-windows-x86_64-cmake-prefix.zip\` |

Each archive has a matching \`.sha256\` sidecar. Pipeline assembles the platform
outputs into one release and adds the aggregate \`SHA256SUMS\` manifest. GitHub also
provides the tagged source code as zip and tar.gz downloads.

The existing user-download archive retains its historical top-level
\`cdin-kernel-VERSION/\` directory and installer workflow. The additional Registry
archive has no wrapper directory: \`include/\`, \`lib/\`, \`bin/\`, \`share/\`, and
\`BUILD-INFO.txt\` are at the archive root. This is the install prefix required by
Registry's \`cmake-prefix-v1\` layout. Both archives are produced from the same
verified package tree, and shared-library aliases are regular files in both.

## Registry publication

After all five matrix targets finish, Pipeline downloads their release outputs into
one assembly workspace. The Kernel-owned
\`scripts/generate_registry_metadata.py\` validates the exact five flat Registry
archive names, their sidecars, source tag/commit identity, package version, C ABI
version, archive structure, target ABI, required headers/libraries and actual
SHA-256/byte count.

It then produces \`kernel-VERSION.json\` with Registry Component Release Metadata
schema 1.2.0. The generated file is placed under \`release-build-metadata/\` so
Pipeline's release assembler treats it as a release-build output, flattens it
alongside the platform packages, and includes it in the final \`SHA256SUMS\`.
The document records the exact full source commit, the provided
\`org.cdin.kernel-abi\` contract, and five target-specific
\`native-development-package\` artifacts using the \`cmake-prefix-v1\` layout.
GitHub's Release Assets API supplies the generated metadata asset's own digest
and size; the JSON does not attempt to hash itself.

The metadata asset is generated after the five native build artifacts are
assembled. It is not committed under \`registry/releases/\`, and an already
published metadata document is never retargeted. Catalog discovery maps the
stable \`kernel/default\` identity to this repository once; Registry resolves
versions from the complete eligible Release inventory.

## Build and verification contract

Each matrix runner executes \`scripts/ci-check.sh\` and \`scripts/ci-package.sh\`.
The package builder calls \`scripts/package-release.sh\`, which creates the
existing user archive and the additional flat Registry archive. It verifies
both checksum sidecars and validates the installer from each extraction layout.
The Registry archive test installs from its root and compiles static and shared
consumers against the installed prefix.

The release workflow waits for all five matrix targets, assembles both archive
families and their checksums, generates the immutable Registry metadata asset,
verifies release checksums, and only then publishes the GitHub Release.

## Historical release layout

Existing releases such as
[\`v1.2.2\`](https://github.com/cdin-editor/kernel/releases/tag/v1.2.2)
remain unchanged. New Registry metadata applies only to releases published
after this workflow change; historical release assets and the checked-in
\`registry/releases/kernel-1.2.1.json\` snapshot remain immutable.
