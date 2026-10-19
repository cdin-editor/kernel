<!--
Thanks for improving cdin-kernel.
This repository is a C11 library with a C ABI. A good pull request here
changes code, vectors or docs, and says exactly which.
Delete any section that does not apply, but do not delete the checklist.
-->

## Summary

<!-- One or two sentences: what changes and why. -->

## Motivation

<!-- What is wrong or missing today? Link the evidence: an issue, a spec requirement. -->

Closes # (issue)
Related requirements: <!-- e.g. DOC-001, EDIT-002 -->

## Type of change

<!-- Tick every box that applies. -->

- [ ] **Bug fix**: fixes a crash, leak or incorrect result (PATCH)
- [ ] **New feature**: new API or capability (MINOR)
- [ ] **Breaking change**: removes or changes existing API (MAJOR, needs owner approval)
- [ ] **Docs**: documentation, comments, README
- [ ] **Build/CI**: Makefile, workflows, scripts
- [ ] **Test**: conformance vectors, fuzz targets
- [ ] **Performance**: optimization with benchmark evidence

## What changed

<!-- File by file. Be specific enough that a reviewer can find each change. -->

| File | Change |
| --- | --- |
|  | |

## Requirements

<!-- List requirement IDs touched. -->

- [ ] Every touched requirement has a conformance vector
- [ ] Relevant user / ABI / developer docs updated
- [ ] No requirement marked `done` unless `make check` and `make asan` pass

## How was this checked?

<!-- Paste the output, or say what you could not run and why. -->

```text
$ make check
```

```text
$ make asan
```

Not run, and why:

## Reviewer notes

<!-- Where should the reviewer look hardest? What are you unsure about? -->
