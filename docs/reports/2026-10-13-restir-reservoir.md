# CPU direct-light reservoir foundation

Follow-up: [GPU initial candidates and lighting evidence](2026-10-13-restir-initial.md).

Measured on 2026-10-13 against `c1f59ab` plus LOTUS-RESTIR-01, with MSVC
Release in the existing OpenStrata core and runtime-free/Vulkan-disabled
configurations. The physical-GPU regression configuration uses the Windows
workstation's NVIDIA RTX A5000. The new reservoir checks themselves are CPU
only and require no Vulkan or OpenUSD.

The [reservoir reference](../reference/RESTIR.md) owns behavior and limits;
the [roadmap](../roadmap/README.md#status-at-a-glance) owns delivery status.
These are arithmetic measurements, not rendered ReSTIR quality or performance
measurements.

## Independent expectations

CTest `lotus-renderer-reservoir` enumerates every proposal draw and each
retain/replace branch of small finite streams. Branch probabilities come
from a separate double-precision prefix sum rather than from the reservoir
implementation. Inputs have binary-exact weights. The checks use:

- Target `(1, 3, 0)` over three samples, with proposal A `(0.25, 0.25, 0.5)`
  and stream lengths 1 through 4.
- RGB contributions `(2, 1, 0.5)`, `(1, 4, 2)` and `(0, 0, 0)`, whose exact
  discrete integral is `(3, 5, 2.5)`. The target differs from the contribution
  and has a zero-target sample.
- The same unshadowed target with partially transmitted first contribution
  `(0.5, 0.25, 0.125)` and a fully occluded second contribution. The integral
  is `(0.5, 0.25, 0.125)`, with visibility applied after selection and no
  change to the normalization.
- A two-draw stream under proposal A combined with a three-draw stream under
  proposal B `(0.5, 0.25, 0.25)`, with the same target and sample domain.
  Both RGB fixtures retain the exact expected integral; the combined count
  is five even when a source has no positive-weight sample.
- An all-zero target and contribution with three draws, returning zero.

All expectations passed the absolute `2e-12` per-channel tolerance, with
total outcome probability within `2e-13` of one. This tests normalization
and candidate counts independently of a noisy rendered image. It does not
establish cross-surface reuse correctness.

## Weighted selection and boundaries

The frequency test streams fixed weights `(1, 2, 7)` through 200,000
independent trials. It uses the renderer's 32-bit PCG construction, starting
with seed `0x6A09E667`, with a separate variate for each update. Results were
identical in both build configurations:

| Selected sample | Expected probability | Measured frequency |
| --- | --- | --- |
| 0 | 0.1 | 0.099295 |
| 1 | 0.2 | 0.201045 |
| 2 | 0.7 | 0.699660 |

Each passed the absolute 0.005 tolerance. Deterministic boundary checks also
passed: empty/all-zero streams, the first positive sample, the strict
replacement boundary, selected-target replacement/retention, zero draws at
`u=0`, unequal/empty/zero stream combination and self-aliasing. Extreme-value
checks cover float normalization denominator overflow, a representable
subnormal and the largest uint32 count. Invalid inputs, malformed records,
unrepresentable candidate weights, sum overflow and count overflow reject
without publishing destination changes.

## Commands and regression evidence

Commands run from the repository root:

```sh
ost build --jobs auto
build/cy2026-windows-x86_64-py313-core/validation/lotus-reservoir-test.exe
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost test
ost validate --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

Both builds compiled the new sources without warnings. Runtime-free CTest
passed 14 checks, with the GPU-only temporal check explicitly skipped (15
registered). The physical-GPU core suite passed 24/24, including the new
reservoir arithmetic, existing reference/wavefront/NEE/MIS transport,
temporal rendering, install-tree rendering, benchmark contracts, memory and
acceleration checks, missing-driver handling and synchronization validation.
Strict renderer evidence validation passed, retaining the core-only Hydra
capability SKIPs. Documentation and Python documentation/release regressions
passed. No Hydra-specific source or rendering behavior changed, and no Hydra
build is claimed by this run.

Build and test logs are retained under `build/reservoir-core-*.log` and
`build/reservoir-runtime-free-*.log`. The test prints the frequency values
and PASS/FAIL results directly.

## Scope disposition

This slice supplies an internal CPU reservoir record, candidate update,
normalization and same-target stream combination. The header is not installed.
The sample is an opaque caller-owned key, with no scene-table identity or
light reconstruction contract. No GPU pass, reuse state, render setting or
public API is added. The existing reference and NEE/MIS paths remain the
transport baselines; arithmetic checks make no convergence or speed claim.

GPU candidates, selected-sample visibility, BSDF-emission accounting,
support-aware temporal/spatial reuse and Hydra/exit measurements remain in
the [execution plan](../roadmap/current.md#execution-order). The shared temporal
infrastructure contains no ReSTIR-specific state.
