# Registry release metadata

Kernel's Registry identity is `kernel/default`. The checked-in
[`kernel-1.2.1.json`](releases/kernel-1.2.1.json) is a historical immutable source
metadata snapshot for package version `1.2.1`; it is retained for existing Catalog
and Manifest identities and must not be rewritten.

New tagged releases publish a generated `kernel-VERSION.json` GitHub Release Asset.
The asset is generated only after all five target archives and their checksum
sidecars have been assembled and verified. It declares:

- Component identity: `kernel/default`.
- Package version: the exact SemVer parsed from the `vVERSION` release tag.
- Source identity: the exact tag-resolved full Git commit in `kernel-source`.
- Provided contract: `org.cdin.kernel-abi`, read from the release's public header.
- Five immutable `native-development-package` artifacts using the
  `cmake-prefix-v1` layout, each with its exact OS/architecture/ABI, URI, byte
  size and SHA-256.
- Build recipe, toolchain and runner provenance recorded in each package.

GitHub's Release Assets API supplies the metadata JSON asset's own digest and byte
size. The metadata document therefore does not embed its own digest. Registry
discovers `kernel/default` through the Catalog's stable GitHub Release Source
mapping; it does not scan this source directory or depend on a hardcoded file path.

See [GitHub Release assets](../docs/github-releases.md) and
[`scripts/generate_registry_metadata.py`](../scripts/generate_registry_metadata.py).
Registry remains a build-time resolver/source collector, not a Kernel runtime
dependency.
