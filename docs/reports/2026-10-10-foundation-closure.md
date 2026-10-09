# Foundation closure review

Reviewed on 2026-10-10 against the
[foundation scope](../design/ROADMAP_POLICY.md#renderer-phase-0--foundation),
the capability matrix and the implementation at `23f12b9` with the
[memory-pool change](2026-10-10-gpu-memory-pools.md). Phase and release status
remain in the [canonical roadmap](../roadmap/README.md#status-at-a-glance).

## Hosted follow-up

The synchronization driver-probe fix's
[main run at `cd22a2b`](https://github.com/animu-sphere/hydra-lotus/actions/runs/37878671481)
succeeded on 2026-10-09: core **8/8**, Hydra **27/27** CTest entries with no
failures. The core and Hydra jobs both completed successfully. This resolves
the hosted-rerun gap in the [earlier report](2026-10-09-synchronization-ci.md).

Both `core-windows-evidence` and `hydra-windows-evidence` artifacts were
downloaded and their `renderer-report.json` inspected. Core boundary and
install-tree assertions passed in both. The runtime-free core report's GPU
and synchronization SKIPs explicitly state that Vulkan was not compiled.
The Hydra report's GPU SKIP records `vkCreateInstance failed with VkResult
-9`; its synchronization SKIP carries the same driver explanation. The
synchronization CTest skipped with the driver-probe explanation; the
simulated missing-driver regression passed. usdview correctly skipped from
that headless capability evidence. Hosted success establishes capability
handling, not physical-GPU rendering.

## Scope dispositions

| Foundation subject | Disposition and evidence |
| --- | --- |
| Host-neutral renderer core, world/extraction, dependency boundary | Implemented; core boundary and discovery tests, scene mesh/update tests, and runtime-free install tree pass. See [layout](../architecture/PROJECT_LAYOUT.md#4-dependency-directions). |
| Vulkan initialization, validation and synchronization | Implemented; physical RTX A5000 strict evidence passes with zero messages, intentional missing-barrier hazard capture passes, unavailable-driver SKIPs verified. |
| Slang pipeline and headless products | Implemented; shader compilation, color/depth readback and unchanged-frame persistence pass, with deterministic reference evidence. |
| Hydra discovery, delegate, buffers, camera and AOV bootstrap | Implemented; Hydra discovery/creation, CPU buffers, AOV restoration, determinism, first frame and stable update all pass on physical GPU. ID sentinels are an accepted bootstrap limitation under the [AOV contract](../reference/AOVS.md#channels), because hit IDs and picking are outside the required color/depth products. |
| Standalone viewport | Implemented bootstrap; `ost renderer viewport -- --frames 8 --hidden` presented 8 frames on RTX A5000 with zero validation messages; `ost validate --intent renderer-viewport` passed. Scene presentation remains the accepted [bootstrap limitation](../reference/SUPPORTED_CONFIGURATIONS.md#foundation-bootstrap-limitations). |
| CI and runtime-free operation | Implemented with hosted capability evidence above and local strict physical-GPU validation. CI acceptance and renderer capability reporting remain separate. |
| OpenStrata workflow generation and runtime-free validation selection | Accepted tooling limitation: the repository-owned workflow validates the CI contract and runtime-free evidence directly; plugin descriptors are not this renderer's responsibility. [Supported configurations](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations) owns the workaround and rationale. |
| Japanese MSVC header dependency discovery | Accepted tooling limitation with explicit clean-rebuild workaround; local builds of affected public-header consumers and all Hydra tests pass. This does not authorize trusting an incremental build with missing dependency records. See [supported configurations](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations). |
| Broader light, material and temporal channels | Deferred to the named scopes in [current.md](../roadmap/current.md#deferred-adapter-work) and the [bootstrap limitations](../reference/SUPPORTED_CONFIGURATIONS.md#foundation-bootstrap-limitations); they are outside foundation bootstrap. |

The headless, Hydra and viewport entry points all start the shared core and
Vulkan backend target. Local validation in this review used Windows 11,
MSVC 14.51, OpenStrata 0.23.14, Release, RTX A5000, and OpenUSD 26.08/Python
3.13 for Hydra. Core **13/13**, Hydra **30/30** and runtime-free **8/8** passed;
commands and physical-GPU results are in the [memory-pool report](2026-10-10-gpu-memory-pools.md).
The viewport launch was checked separately, without claiming its CTest
suite ran. Headless JSON, reference images and usdview screenshots remain
in their respective build trees. This review does not establish a fixed
performance baseline or a shipped release.
