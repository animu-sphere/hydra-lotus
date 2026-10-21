# CPU differing-target reservoir arithmetic

Measured on 2026-10-13 against `a013ac8` plus the CPU arithmetic portion of
LOTUS-RESTIR-03, MSVC Release, Windows. The
[reservoir reference](../reference/RESTIR.md#differing-target-cpu-combination)
owns the operations and caller requirements. This run establishes an oracle
for receiving-surface reevaluation and support normalization; it adds no
GPU history pass or moving-sequence result.

## Independently enumerated integrals

`lotus-renderer-reservoir` enumerates every draw and retain/replace branch
of two source streams over three discrete samples. The streams contain two
and three draws, with proposal probabilities `(0.25, 0.25, 0.5)` and
`(0.5, 0.25, 0.25)`. Source target pairs are:

| Case | Source A | Source B |
| --- | --- | --- |
| Different full targets | (1, 3, 2) | (4, 2, 1) |
| Complementary support | (1, 0, 2) | (0, 2, 1) |
| Entirely zero source | (1, 3, 2) | (0, 0, 0) |
| Partly supported source | (1, 3, 2) | (0, 2, 0) |

Each pair is combined at receiving targets `(2, 1, 4)` and `(2, 0, 4)`,
with per-source candidate caps 1, 2, 3 and 8, in both merge orders: 64 cases.
The oracle computes receiving selection probabilities from independently
checked source sums, targets and represented counts. It enumerates both
selection branches rather than using stochastic sampling to approximate
the expectation. Every merged represented count matches the capped counts.
Resolved normalization agrees with double reference arithmetic within
`2e-7 * max(1, reference W)`, allowing float weight/sum storage.

Unshadowed RGB contributions are `(2,1,0.5)`, `(1,4,2)` and `(3,0.5,1)`.
A second integral multiplies them by visibility 0, 0.25 and 1. Contributions
at a zero receiving target are zero. Every expected RGB channel agrees with
the independently summed discrete integral within 2e-6. The test includes
observed all-zero streams from targets that still support the final selected
sample; their counts remain in that sample's support denominator.

Using the total represented count instead of the supporting count produces
absolute red-channel error up to 3.6 in these same fixtures. The test requires
this negative comparison to exceed 1, so its fixtures detect the missing
support correction rather than merely confirming a shared formula.

## Boundaries and numerical failures

Checks cover receiving reevaluation, simultaneous weight/count scaling under
a cap, retained zero-target draws, empty and entirely zero sources, all-zero
receiving merges, strict replacement equality and source/destination aliasing.
A previously support-corrected weight is passed explicitly through another
capped merge; reconstructing it from the raw record would fail this check.
A source normalization beyond float range still succeeds when multiplied by
a tiny receiving target to give a representable merge weight.

Invalid targets, source weights, random variates, caps, malformed records and
support counts are rejected. Float receiving-weight overflow/underflow,
double intermediate overflow, sum overflow and represented-count overflow
are rejected with unchanged destination records. These are arithmetic tests;
they do not check persistent scene/light identities or temporal publication.

## Regression commands

Commands run from the repository root:

```sh
ost build --jobs auto
ost test
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

Core passed 26/26 CTests, including the existing physical-GPU initial-lighting
and temporal checks, installed rendering, reference-image comparisons and
synchronization validation. Strict renderer-evidence validation passed;
core-only Hydra assertions retained their explained SKIPs. Runtime-free
checks passed 14 tests, with temporal and reservoir GPU checks explicitly
skipped (16 registered). The documentation checker and all 27 Python
documentation/release tests passed.

After adding one final all-zero receiving-merge boundary assertion, both
configurations were rebuilt and the reservoir test was rerun with:

```sh
ctest --test-dir build/cy2026-windows-x86_64-py313-core -R '^lotus-renderer-reservoir$' --output-on-failure -V
ctest --test-dir build/cy2026-windows-x86_64-py313-core-runtime-free--ci-core -R '^lotus-renderer-reservoir$' --output-on-failure -V
```

Both focused runs passed and printed the 64 integral cases and negative
normalization comparison above. OpenStrata full-run build/test logs remain
under `.strata/targets/`; focused CTest output is in each build tree's
`Testing/Temporary/LastTest.log`. This private CPU change introduces no Hydra
control or shader behavior, and no additional Hydra sequence is claimed.

## Scope disposition

The CPU combination and normalization oracle implements the arithmetic
prerequisite of LOTUS-RESTIR-03. GPU reprojection/history integration, receiving
and source surface evaluation, persistent sample identity, reset/failure
publication and moving NEE/MIS comparisons remain in the
[execution plan](../archive/pre-v070-execution-plan.md#execution-order). Source support and
its coverage of the receiving integral remain caller requirements. These
finite discrete fixtures do not establish unbiased adaptive history selection,
recursive moving-scene transport or a temporal performance improvement.
