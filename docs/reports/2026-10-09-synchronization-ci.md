# Synchronization validation on a host without a Vulkan driver

Observed on 2026-10-09 for PR #27, based on `d880443`. The
[initial hosted run](https://github.com/animu-sphere/hydra-lotus/actions/runs/37787203501/job/113344900567)
failed only `lotus-renderer-synchronization-validation` with
`vkCreateInstance failed with VkResult -9` (`VK_ERROR_INCOMPATIBLE_DRIVER`).
The Windows 2022 Hydra job installed Vulkan SDK 1.4.350.0; existing GPU and
usdview tests correctly skipped unavailable GPU capability. The core job
passed. The new test had treated instance creation without a usable driver
as an ordinary setup failure.

## Fix and regression

Probe Vulkan 1.3 instance creation without layers before the synchronization
test enables the shared validation setup. An incompatible driver now prints
an explicit reason and exits 77. Other probe and validation setup failures
remain errors. Actual synchronized writes and the intentionally missing
barrier remain required when driver and validation support are available.

The Vulkan-only `lotus-renderer-synchronization-no-driver` CTest script
restricts both `VK_DRIVER_FILES` and `VK_ICD_FILENAMES` to a nonexistent
manifest in its child process and requires exit 77 plus
`SKIP: ... VK_ERROR_INCOMPATIBLE_DRIVER`. It runs the executable directly
to preserve its exit code. The override follows the
[Khronos loader's driver discovery contract](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#overriding-the-default-driver-discovery).
Installed drivers and the parent test process are unaffected.

Local verification used Windows x86_64, MSVC 14.51, OpenStrata 0.23.14,
Release builds, and the NVIDIA RTX A5000. Hydra used OpenUSD 26.08 lookdev
and Python 3.13. The synchronized-write/missing-barrier test and the new
no-driver regression both passed. This complements the original
[synchronization evidence](2026-10-08-synchronization-validation.md).

## Commands and results

- `ost build --jobs auto`, `ost test`: **10/10 passed**.
- `ost validate --strict-renderer-evidence`: passed.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **27/27 passed**, including
  usdview, the synchronization hazard test and the no-driver regression.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passed.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**.
