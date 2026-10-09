# AOV buffer restoration — 2026-10-09

## Problem and implementation

`LOTUS-AOV-01` required resolving the no-clear buffer-switch limitation.
The adapter previously wrote the preceding GPU attachment into every newly
bound CPU buffer. An A/B/A switch could copy B's background into A; a host
write or same-size reallocation could likewise be replaced with old pixels.

The adapter now reads the currently bound unmapped CPU colour/depth buffers
when their clear values are empty, converts them into top-left float inputs,
and restores those inputs before drawing. The backend reuses its persistent
readback buffers as staging and synchronizes upload, attachment access and
readback. Restoration occurs only before the first submission in a render
call. The radiance accumulation is independent of restoration contents.
Current semantics and the synchronous copy/upload cost are owned by the
[AOV reference](../reference/AOVS.md#clears-and-successive-frames).

## Configuration and checks

Measured locally on Windows x86_64, MSVC 14.51, Release, OpenStrata 0.23.14,
Vulkan on NVIDIA RTX A5000. The Hydra build used OpenUSD 26.08 lookdev and
Python 3.13. The Hydra test's scene is its index-matched Lambert triangle/quad
under the white fallback environment, at 16 × 16, sample index 0. Empty-scene
restoration uses distinct per-row patterns in 8-bit and float colour, including
negative/HDR values, and varying depth. Resize checks use an 8 × 12 target.

- `lotus-renderer-hydra-render-buffer`: top-down restoration rows, UNORM
  widening, float preservation, wrong-format and mapped-buffer rejection.
- `lotus-renderer-hydra-aov`: first no-clear binding, A/B/A switches between
  colour formats, host writes, depth-only interleaving, resize and return,
  same-size reallocation. At four converged samples, restored backgrounds
  survive misses while hits still write 0.18 radiance and centre depth,
  without restarting accumulation.
- `renderer.aov.clears` in headless evidence: patterned colour/depth
  restoration through the bootstrap for three submissions, unchanged target
  creation count, preservation outside a small data window, and rejection of
  a malformed restoration extent.
- An additional AOV CTest run with `LOTUS_HYDRA_EVIDENCE` captured each frame's
  synchronization enablement and validation-message count: all **22 frames**
  enabled synchronization validation and captured **zero messages**.

## Commands and results

- `ost build --jobs auto`, `ost test`: **10/10 passed**.
- `ost validate --strict-renderer-evidence`: passed, including deterministic
  reference images and zero Vulkan validation messages.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **27/27 passed**, including
  AOV checks, deterministic Hydra comparisons, usdview and synchronization.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passed.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**, with explained
  unavailable GPU capabilities.
- `python scripts/check_docs.py`: passed;
  `python -m unittest discover -s scripts -p test_check_docs.py`: **14/14 passed**.

These are local results. The separate hosted CI task remains open; no hosted
execution or performance improvement is claimed by this report.
