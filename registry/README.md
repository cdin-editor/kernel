# Registry release metadata

Kernel publishes its component Release Metadata in `registry/releases/`. The current entry is `kernel-1.2.1.json`, with source revision `f4875b6eb59bd76878e7ff486b6114abd9bb1329`.

- Component identity: `kernel/default`
- Package version: `1.2.1`
- Provided contract: `org.cdin.kernel-abi` version `1.1.0`
- Dependencies: none

A new Kernel release must publish a new immutable metadata document that points to the full source commit and declares the ABI contract version. Do not edit a published metadata document in place. Registry is a build-time resolver/source collector, not a Kernel runtime dependency.
