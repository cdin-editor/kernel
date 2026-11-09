# GHCR package distribution

The kernel is published through **both** GitHub Releases and GitHub Container Registry.
GitHub Releases keep their normal downloadable archives; GHCR publishes the same
per-platform package archives as OCI artifacts.

These are package archives, not runnable container images. Use ORAS to download them;
`docker pull` is for container images and is not the correct command for this package.

## One GHCR package, five platform tags

Pushing a version tag such as `v1.2.1` runs
[`.github/workflows/oci-release.yml`](../.github/workflows/oci-release.yml).
The workflow reuses the release build/package scripts and publishes all five targets
to the same package repository:

```text
ghcr.io/cdin-editor/cdin-kernel:1.2.1-linux-x86_64
ghcr.io/cdin-editor/cdin-kernel:1.2.1-linux-arm64
ghcr.io/cdin-editor/cdin-kernel:1.2.1-macos-x86_64
ghcr.io/cdin-editor/cdin-kernel:1.2.1-macos-arm64
ghcr.io/cdin-editor/cdin-kernel:1.2.1-windows-x86_64
```

Each tag is a separate OCI artifact in the **same** `cdin-kernel` GHCR package.
The OCI metadata records the component, version, source commit, target OS,
architecture, ABI, archive filename, size, media type, and SHA-256. Pipeline
creates an immutable manifest digest and a publication receipt for every target,
then pulls each artifact back by digest and verifies the downloaded bytes.

Linux and macOS publish the normal `.tar.gz` release archive. Windows publishes
the normal `.zip` archive and declares its layer media type as `application/zip`.
The platform identifiers and archive naming match the GitHub Release assets.

## Download a target

Install [ORAS](https://oras.land/) and pull the platform tag you need:

```sh
oras pull ghcr.io/cdin-editor/cdin-kernel:1.2.1-linux-arm64
```

For Windows, use:

```sh
oras pull ghcr.io/cdin-editor/cdin-kernel:1.2.1-windows-x86_64
```

For verification, use the immutable digest reported in that target's workflow
publication receipt rather than relying only on a mutable tag.

## Visibility

After the first publication, confirm the `cdin-kernel` package is linked to this
repository and set its package visibility to **Public** in the package settings.
Publishing with `GITHUB_TOKEN` grants workflow write access; it does not itself
guarantee that an existing package's visibility has been changed to public.

## Build and test guarantees

All five jobs run natively on their corresponding GitHub-hosted runner. They execute
`scripts/ci-check.sh` and `scripts/ci-package.sh`; that package script installs the
pinned `m-mdy-m/log.c` dependency, builds the kernel package, validates its checksum,
and runs the release archive installation/consumer tests before the artifact is staged
and published.

This OCI package is independent from the CDIN application and does not combine
kernel, runtime, backend, or frontend into a container image.
