# Contributing to Hydra Lotus

Contributions to code, documentation, tests and reproducible bug reports
are welcome. Participation follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

Read the [design policy](docs/design/DESIGN_POLICY.md),
[project layout](docs/architecture/PROJECT_LAYOUT.md) and
[current work](docs/roadmap/current.md). The
[capability matrix](docs/reference/CAPABILITY_MATRIX.md) describes existing
behavior; the [roadmap](docs/roadmap/README.md) owns delivery status.

For substantial work or a public API change, open an issue or draft pull
request first to discuss the problem, ownership and expected evidence.
Use the [bug report](.github/ISSUE_TEMPLATE/bug_report.yml),
[feature request](.github/ISSUE_TEMPLATE/feature_request.yml) or
[documentation issue](.github/ISSUE_TEMPLATE/documentation.yml) form.
For a suspected vulnerability, follow [SECURITY.md](SECURITY.md).

## Development setup

Follow [BUILDING.md](docs/guides/BUILDING.md) for prerequisites, runtime
selection and plain CMake. The default build uses OpenStrata (`ost`),
C++20, CMake and Vulkan/Slang, with no OpenUSD dependency. A missing Vulkan
SDK produces explained GPU SKIPs. The Hydra adapter needs a matching
OpenUSD imaging runtime.

The default validation loop is:

```sh
ost build --jobs auto
ost test --jobs 8
ost validate --strict-renderer-evidence
```

For changes affecting Hydra, also run:

```sh
ost build --profile lookdev --intent hydra --jobs auto
ost test --profile lookdev --intent hydra
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
```

For host-neutral changes, verify the runtime-free configuration:

```sh
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
```

For changes to code a closed research campaign measured, such as the
reservoir passes or the reservoir benchmark, also run the research tier:

```sh
ost build --intent research --jobs auto
ost test --intent research --jobs 8
```

Every new CTest takes one tier with `lotus_test_tier`; the
[test policy](docs/design/TEST_POLICY.md) says which, and what a permanent
test must justify.

Report missing capabilities and checks you could not run. A GPU or runtime
SKIP explains coverage; it does not establish rendering correctness.
The build guide documents the Windows incremental-header rebuild limitation.

## Change guidelines

- Keep scene state and extraction host-neutral. Put Hydra translation under
  `adapters/hydra2/` and Vulkan execution under `backend/vulkan/`, following
  the [dependency boundaries](docs/architecture/PROJECT_LAYOUT.md).
- Preserve deterministic reference rendering. Check faster modes against
  the reference path tracer and explain intentional reference-image changes.
- For rendering changes, include the scene, settings, sample count, seed
  and image comparisons needed to assess correctness. For performance
  changes, include measured GPU stages and workloads; use the
  [fixed benchmark procedure](docs/guides/BUILDING.md#fixed-performance-baseline).
- Add or update relevant tests for changed behavior. Explain when a
  documentation or other low-impact change needs no runtime tests.
- Return actionable diagnostics for unsupported input or unavailable
  capabilities. Preserve explicit PASS, FAIL and SKIP evidence.
- Follow the [integration scope](docs/design/INTEGRATION_SCOPE_POLICY.md):
  consume standard scene/material representations and keep sibling renderers
  and source-format plugins outside the dependency graph.
- Update the owning documentation when commands, public boundaries,
  capabilities or limitations change. Record user-facing changes under
  Unreleased in [CHANGELOG.md](CHANGELOG.md), when applicable.
- Include only scenes and assets you may redistribute, with their required
  licenses and provenance. Remove credentials and private scene data from
  logs and attachments.

Follow the [documentation guidelines](docs/contributing/documentation.md)
and run these checks for documentation changes:

```sh
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

## Pull requests

Keep changes focused and use the
[pull request template](.github/PULL_REQUEST_TEMPLATE.md) to describe the
problem, resulting behavior, affected configurations and verification.
List commands and results, including relevant install-tree checks. Link
images or benchmark evidence for rendering/performance work and identify
compatibility, packaging or public API implications. Explain any checks
that were not run and why.

## License

Unless a file states otherwise, contributions to this repository use its
[Apache License 2.0](LICENSE). Retain applicable third-party notices.
