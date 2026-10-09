# Vulkan synchronization validation

Measured on 2026-10-08 in the working tree based on `170ced5`, on Windows
x86_64 with MSVC 14.51 and OpenStrata 0.23.14, Release builds, and an NVIDIA
RTX A5000 (reported Vulkan API 1.4.329). Hydra used OpenUSD 26.08 lookdev
and Python 3.13. These were local runs; hosted CI was not measured.

## Scope

The testing-infrastructure follow-up from the roadmap: explicitly enable
synchronization validation, capture its messages with the existing debug
messenger, and report unavailability without claiming a clean validation run.

The shared instance helper enumerates the Khronos layer's extensions
separately from the loader's global extensions, enables
`VK_EXT_validation_features`, and chains `VkValidationFeaturesEXT` with
`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` into instance
creation. This follows the
[Vulkan extension enumeration contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkEnumerateInstanceExtensionProperties.html)
and [validation feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkValidationFeaturesEXT.html).
Offscreen rendering (headless and Hydra) and swapchain presentation use the
same helper. Current capability belongs to the
[capability matrix](../reference/CAPABILITY_MATRIX.md).

## Evidence

`lotus-renderer-synchronization-validation` records two overlapping
`vkCmdFillBuffer` writes to a 256-byte buffer with a transfer-write barrier:
zero messages. It resets the command buffer, records the same writes without
the barrier, and requires exactly one captured message containing
`SYNC-HAZARD-WRITE-AFTER-WRITE`. Neither command buffer is submitted, so the
negative case does not execute a data race. The test returns CTest SKIP 77
when synchronization capture or a suitable device is unavailable, and is
omitted from Vulkan-disabled builds.

The ordinary headless suite passed `renderer.validation.synchronization`
with zero messages across scene uploads, acceleration builds and refits,
texture edits, readbacks, repeated frames and rendering. The usdview host
log contains 261 completed Hydra frames, each with
`synchronization_validation=1` and `validation_messages=0`, including scene
updates and an AOV resize. Its Python assertions also require this
enablement when the headless report says synchronization validation is
available.

The Cornell box regression passed at 1, 16, 64, 256 and 1024 spp, starting
at sample index `1 << 20`, with the existing 64-bounce limit. Rendering its
original sequence (sample index 0, 1024 spp) reproduced the committed mean
bit for bit. No scene, shader or reference image changed.

## Commands and results

- `ost build --jobs auto`: passed.
- `ost test`: **9/9 passed**, including the new synchronization test.
- `ost validate --strict-renderer-evidence`: passed.
- `ost build --profile lookdev --intent hydra --jobs auto`: passed.
- `ost test --profile lookdev --intent hydra`: **26/26 passed**, including
  usdview and the new synchronization test.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passed.
- `ost build --without-runtime --intent ci-core --jobs auto` and
  `ost test --without-runtime --intent ci-core`: passed, **8/8 tests**.
  `renderer.validation.synchronization` is an explained SKIP because the
  Vulkan backend was not compiled.
- `ost renderer viewport -- --frames 8 --hidden`: passed, eight frames on
  the RTX A5000 with zero captured validation messages. The documented
  Japanese-MSVC header-dependency workaround was needed: two viewport objects
  had `#deps 0` and were removed before rebuilding against the changed public
  statistics header.

Unavailable validation layers or extension support retain ordinary rendering
and report the reason. That fallback is implemented; only the Vulkan-disabled
unavailability case was measured in this run.
