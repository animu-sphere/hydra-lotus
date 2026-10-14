# Hydra direct-light reservoir controls

Measured 2026-10-14 against `c943716` plus this Hydra reservoir change, with
MSVC Release, Vulkan/Slang, NVIDIA RTX A5000 and the OpenUSD 26.08 lookdev
runtime on Windows. The
[reservoir reference](../reference/RESTIR.md#hydra-controls-and-diagnostics)
owns controls and limits. The [roadmap](../roadmap/README.md#status-at-a-glance)
owns phase/release status.

## Contract and moving USD stage

`lotus-renderer-hydra-restir-contract` checks all six setting defaults and
six AOV descriptors without creating a GPU renderer. Synthetic 2x2 products
check spatial/temporal/initial stage priority, explicit corrected normalization,
bottom-up row layout, disabled-mode sentinels and mapped/short/invalid-extent
write rejection.

`lotus-renderer-hydra-restir-gpu` composes a perspective USD camera, floor,
wall and tilted nonemissive panel with a fixed parallel light (intensity 2,
RGB 3/2/1, angle zero). Both integrators render at 32x32, sample index 4096,
two initial candidates and source cap three. Initial-only transport converges
at three spp; temporal/spatial modes trace one fresh sample per pass.
Every final diagnostic record is compared bit for bit to GPU products,
including corrected weights and shadowed direct contributions.

The sequence exercises static temporal reuse, 0.12 X camera translation,
0.3 X panel translation, temporal/spatial combination, spatial-only mode,
independent radiance accumulation, integer bounds, pending cuts after rejected
colour bindings and render-pass switching. Returning to progressive NEE/MIS
reproduces the deterministic colour image. After convergence, newly allocated
and rebound debug buffers recover the preceding fresh diagnostic records
without generating another reservoir or changing colour.

## Installed usdview

The installed host test retains its existing deterministic, integrator,
NEE/MIS and temporal-radiance checks. It adds a fixed USD parallel light,
four-spp progressive batches, two candidates and source cap eight. Under
each integrator, it switches through initial, temporal, combined and spatial
reservoir modes, camera/object motion, explicit cuts and progressive NEE/MIS
restoration. It counts actual frame completions for continuous modes and
keeps screenshots under `usdview-install/`.

Host evidence includes effective estimator/reuse settings, reservoir availability,
history and bytes. Existing checks require zero synchronization messages and
retained renderer/targets. The child deadline is 180 seconds and CTest's
deadline is 210 seconds to accommodate the additional installed sequences.

The first full-suite host run stopped at 63/64 samples in the preexisting
fixed-lighting progressive step, before enabling ReSTIR, and hit its
180-second child deadline. The helper now explicitly requests paints and
uses a per-step deadline instead of testusdview's unbounded convergence
wait; continuous-mode frame waits request paints too.

## Verification

- `ost build --profile lookdev --intent hydra --jobs auto`: passes.
- `ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra
  -C Release -R 'hydra-restir|usdview-host' --output-on-failure`: final
  targeted checks pass 3/3. The contract test takes 0.06 seconds, moving
  GPU test 7.62 seconds and installed host 111.81 seconds. Both integrators
  and all four reuse modes complete with zero synchronization messages.
- `python scripts/check_docs.py`: passes.
- `python -m unittest discover -s scripts -p 'test_*.py'`: 27/27 pass.
- `ost test --profile lookdev --intent hydra`: final managed suite passes
  53/53 in 414.76 seconds, including the installed host in 109.64 seconds,
  temporal/reservoir GPU regressions, deterministic references, install-tree
  checks and missing-driver/synchronization checks.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passes with completion-bound GPU and installed-host evidence. Viewport and
  packaging checks retain their explained SKIPs; this change prepares neither
  a viewport launch nor a release package.

Logs use `build/restir-hydra-` prefixes, including `final-build.log`,
`final-targeted.log`, `final-test.log`, `final-validate.log`, `docs.log` and
`python.log`. The first full suite passed 52/53, with only the host timeout
described above. Its failed managed producer was replaced by the successful
full rerun; a targeted CTest rerun alone did not replace that stored producer.

## Scope

This implements the Hydra controls/diagnostics and installed moving-stage
portion of LOTUS-RESTIR-05. It changes no candidate proposal, reuse arithmetic,
light-domain identity or geometric gate. The existing
[four-mode many-light measurements](2026-10-14-restir-spatial.md#correlations-and-moving-many-light-comparison)
remain the sampling baseline. Candidate-strategy evaluation and broader
convergence/time/memory/bias/artifact measurements, the phase scope review
and release gate remain in the [active execution plan](../roadmap/current.md).
These adapter checks do not establish a many-light speedup or phase exit.
