---
status: accepted
owner: hydra-lotus
---

# Test policy

> Status: **accepted**, 2026-10-18. This document says where each check
> runs, which checks become permanent tests and when they are retired.
> What Lotus tests is the [roadmap policy's testing strategy](ROADMAP_POLICY.md#7-testing-strategy);
> the commands are in the [build guide](../guides/BUILDING.md#test-tiers).

## 1. The rule

> **A test proves a contract continuously. A report proves a research
> conclusion, once, and again only when the implementation it measured
> changes.**

Validation keeps its strength by staying fast where it runs often. A closed
research question is not re-proven on every change; its regression tests
protect the implementation contract the research established.

## 2. Tiers

Every CTest carries exactly one tier label, and `lotus-test-tiers` fails the
suite when a test has none or several.

| Tier | What it is | Without a GPU |
| --- | --- | --- |
| `fast` | Needs no Vulkan device: CPU oracles and arithmetic, option and settings logic, configuration and dependency-boundary contracts, missing-driver behaviour. Same result on every host. | Runs and asserts |
| `gpu` | Renders on a Vulkan device when one is present: a small fixed fixture, a fixed seed, a few frames, validation enabled. One representative case per contract. | Explained SKIP |
| `research` | Re-runs a closed campaign's procedures: workload, proposal and seed sweeps and the controls that keep a measurement honest. Registered only by the `research` build intent. | Explained SKIP |

Area labels (`hydra`, `viewport`, `presentation`, `host-smoke`) are
independent of the tier and select a subsystem with `ctest -L`.

## 3. Where the tiers run

The boundary that matters is whether a GPU is present, not whether a change
is a pull request or a `main` push: the hosted runners have no GPU, and both
events run the same hosted workflow.

| Context | Runs |
| --- | --- |
| Hosted CI — pull requests and `main` | `fast` with real assertions; `gpu` as explained SKIPs. A pull request that changes only documentation skips the builds; `main` pushes, dispatches and release calls always build, so a wrong path classification cannot hide an integration failure. |
| Before a renderer pull request | The default `ost test` (`fast` and `gpu`) on a physical GPU, and the Hydra build when the change reaches Hydra ([contributing](../../CONTRIBUTING.md#development-setup)). |
| A change to code a closed campaign measured | The `research` tier as well. |
| Phase exit, a quality or performance claim, a release | The `research` tier and the campaign scripts the claim needs, recorded as a [report](../reports/README.md); the release gate follows [RELEASING.md](../guides/RELEASING.md). |

## 4. Budgets

Budgets are constraints on the suite's design, not targets to fill.

- **Hosted CI:** about ten minutes for a pull request, almost all of it the
  Hydra build. Test execution there is seconds.
- **Default local suite:** `ost test --jobs 8` on a physical GPU stays near a
  minute. The longest single test bounds it, so a long GPU test shares its
  device across cases or is split, rather than growing. A change that pushes
  the suite well past that is a sign that evidence has leaked into
  regression ([measured](../reports/2026-10-18-test-tiers.md)).
- **Research:** no short budget, but no redundant cases either.

## 5. Admitting a test

Before adding a permanent test, answer:

1. Which regression does it detect, and is that already covered?
2. Can a smaller fixture, one integrator or one seed detect the same failure?
3. Is it a research measurement rather than a test?
4. Which tier owns it, and what does it cost?

A test without a tier is not added. Prefer one representative case per
contract; do not multiply integrator × workload × mode × seed × resolution.
The second integrator belongs in the regression tiers only when the code
under test is not shared between them.

## 6. After a phase closes

Keep the tests that protect the implementation contract: arithmetic, one
GPU fixture per path, reset behaviour, no invalid state, and host plumbing.
Move the campaign's sweeps to `research`, or remove them when nothing can
change their answer. Historical evidence lives in reports, not in the
suite's runtime.

For Renderer Phase 5, reservoir arithmetic and statistics (`fast`), the GPU
reservoir lifecycle, the distant reservoir benchmark under both integrators
and the Hydra reservoir checks (`gpu`) stay; the workload and proposal
variants and the procedure controls are `research`
([reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)).

Consolidate periodically: duplicate fixtures, bootstrap tests superseded by
stronger contracts, and combinations that exist only because a past
campaign needed them.

## 7. Failures, flakiness and timing

- An expected absence — no physical GPU, an unsupported optional feature, an
  unavailable host — is an explained SKIP. A supported configuration fails
  on validation or synchronization errors, a missing compiler or runtime,
  malformed evidence, and deterministic or reference results outside their
  tolerance.
- A flaky test is a bug in the test. Do not retry blindly: find whether the
  cause is timing, the driver, a seed, the environment or the renderer;
  make it deterministic, move true variance out of the regression tiers, or
  remove the test.
- Timing is observed, never a pass threshold, in `fast` and `gpu`.
  Performance conclusions come from controlled workloads at locked clocks
  and are reports ([reuse cost](../reports/2026-10-17-restir-reuse-cost.md)).

## 8. SVGF

Denoising is where the matrix would grow next.

- **`gpu`:** a static fixture and a motion/disocclusion fixture, small, at a
  fixed sample count and seed: moment and variance sanity, history validity,
  finite and bounded filtered output, reset behaviour, clean validation.
  One integrator unless the code under test differs between them.
- **`research`:** the quality study — raw low spp, temporal accumulation
  alone and filtered output against a high-spp reference on RMS/MSE,
  temporal residual, disocclusion response, static convergence and
  moving-camera stability, under both integrators, with enough seeds for the
  conclusion. It is a report campaign, not a permanent matrix.

## 9. Open question

**TST-Q1 — a self-hosted GPU runner.** It would run the `gpu` tier, and the
Hydra installed host, on `main` pushes, where today they run only on a
developer's GPU and at the release gate. Undecided: the cost of operating
it against the regressions a pre-merge local run already catches.
