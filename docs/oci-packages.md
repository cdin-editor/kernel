# OCI package publication

This repository consumes the reusable OCI publisher provided by
[m-mdy-m/pipeline](https://github.com/m-mdy-m/pipeline). The kernel remains
responsible for its own build, tests, install tree, and release archive;
Pipeline owns package identity metadata, content hashing, immutable staging,
registry publication, digest receipts, pull-back verification, and evidence
artifacts.

## Integration smoke package

`.github/workflows/oci-smoke-publish.yml` runs only on pushes to the dedicated
`feat/oci-artifacts-ghcr` integration branch. It uses the existing
`scripts/ci-package.sh` release path, so the OCI payload is the same
installable archive shape used by the GitHub Release workflow rather than an
ad-hoc tarball.

The smoke build:

1. Installs the exact `m-mdy-m/log.c` commit recorded in `deps.lock`.
2. Builds the kernel static/shared libraries and quickstart, then runs the
   conformance vectors, regression tests, and public-header checks.
3. Creates the normal versioned Linux release archive and its SHA-256 file.
4. Installs and executes static and shared consumers from the archive.
5. Checks that the OCI payload contains the kernel and logger headers,
   libraries, pkg-config files, installers, license, and build metadata.

The package is published to:

```text
ghcr.io/m-mdy-m/cdin-kernel-smoke
```

Each run gets a unique SemVer prerelease version based on the workflow run and
attempt. Pipeline derives the platform-specific OCI tag by appending
`linux-x86_64`; consumers should use the immutable manifest digest from the
publication receipt for identity-sensitive pulls.

The reusable publisher is temporarily referenced at
`feat/oci-artifacts-ghcr` in `m-mdy-m/pipeline` while its reusable-workflow PR
is under review. This branch-only opt-in is accepted by Pipeline only for a
push to the exact `feat/oci-artifacts-ghcr` branch with
`allow-branch-publish: true`; the ordinary release workflow is unchanged.
After the publisher PR is merged, update both `uses` and `pipeline-ref` to
`@main` before wiring OCI publishing into tagged production releases.

## Package identity boundaries

The OCI component identity is `cdin-kernel`; the smoke package name is
`cdin-kernel-smoke`. The archive contains the separately built
`m-mdy-m/log.c` static dependency because the kernel's public static-link
contract requires it. The kernel ABI version remains independent from the
package SemVer version and is recorded in `BUILD-INFO.txt`.

This package is a distributable kernel archive, not a CDIN application image
or a combined runtime/backend package. It does not introduce dependency
resolution or product composition into Pipeline.
