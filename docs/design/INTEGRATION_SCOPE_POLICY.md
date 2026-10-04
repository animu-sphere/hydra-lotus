---
status: accepted
owner: hydra-lotus
---

# Integration scope policy

> Status: **accepted**, 2026-10-04. This document says how far `hydra-lotus`
> goes, what it consumes from whom, and what it does not own. On structure,
> [PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md) wins.

## 1. The rule

> **`hydra-lotus` reads the composed USD stage through Hydra and the standard
> schemas, and nothing else.**

It parses no source format, links no format repository's plugin or library,
and reads no project-specific material schema. Material input is
`UsdPreviewSurface`, MaterialX and OpenPBR
([design policy §17](DESIGN_POLICY.md#17-material)); a format repository's
own material semantics reach Lotus only through the standard realizations
that repository authors. Lotus defines no USD schema of its own
([design policy §48](DESIGN_POLICY.md#48-not-done-first)).

## 2. What this repository owns

- The renderer core: `LotusScene`, the update plan, render settings and
  integrator selection ([design policy §4](DESIGN_POLICY.md#4-renderer-core),
  [§52](DESIGN_POLICY.md#52-where-this-repository-departs-from-the-implementation-direction)).
- The Vulkan backend: `GpuScene`, acceleration structures, the traversal
  backends, the integrators, and the Slang shader library — BSDFs, sampling,
  wavefront, ReSTIR, denoising ([§5–§16](DESIGN_POLICY.md#5-gpu-backend)).
- The Hydra adapter (`hdLotus`): scene extraction, dirty tracking, AOVs and
  render settings ([§3.1](DESIGN_POLICY.md#31-hydra-is-a-scene-integration-layer),
  [§32](DESIGN_POLICY.md#32-use-of-openusd--hydra)).
- The material translators and the Lotus material IR
  ([§18](DESIGN_POLICY.md#18-material-ir)).
- The `DenoiserBackend` interface and the denoisers behind it
  ([§16](DESIGN_POLICY.md#16-denoising)).
- Profiling, debug AOVs, the deterministic mode and the golden-image tests
  ([§24–§26](DESIGN_POLICY.md#24-gpu-profiling)).
- The headless runner, benchmark and test-scene runner that drive the core
  without Hydra.

## 3. What it consumes, and from whom

Each subject is linked to its owner and never restated here
([contributing/documentation.md](../contributing/documentation.md)).

| Subject | Owner | Canonical document |
| --- | --- | --- |
| Hydra (`HdRenderDelegate`, `HdMesh`, `HdInstancer`, `HdMaterial`, `HdLight`, `HdCamera`, AOVs, render settings) | OpenUSD | [OpenUSD documentation](https://openusd.org/release/index.html) |
| `UsdGeom`, `UsdLux`, `UsdShade`, `UsdRender`, later `UsdVol` | OpenUSD | [OpenUSD documentation](https://openusd.org/release/index.html) |
| `UsdPreviewSurface` | OpenUSD | [UsdPreviewSurface specification](https://openusd.org/release/spec_usdpreviewsurface.html) |
| MaterialX | Academy Software Foundation | [materialx.org](https://materialx.org/) |
| OpenPBR | Academy Software Foundation | [OpenPBR specification](https://academysoftwarefoundation.github.io/OpenPBR/) |
| Vulkan, ray query and the ray tracing pipeline | Khronos | [Vulkan specification](https://registry.khronos.org/vulkan/) |
| Slang | Khronos (`shader-slang`) | [Slang documentation](https://shader-slang.org/) |
| Build, runtime adoption, renderer evidence and validation | `open-strata` (`ost`) | [adopting a renderer project](https://github.com/animu-sphere/open-strata/blob/main/docs/guides/adopt-a-renderer-project.md) |

## 4. Dependency rules

1. **No link-time edge to a sibling renderer.** `hydra-merlin` and
   `hydra-toon` are references for technique — Vulkan setup, the OpenStrata
   adoption path, Hydra adapter handling across OpenUSD versions — and not
   dependencies. Whether low-level Vulkan utilities are ever shared is
   [design policy §53 DES-Q4](DESIGN_POLICY.md#53-open-questions); the
   renderer core, scene representation, material runtime and Hydra
   synchronization are never shared ([§30](DESIGN_POLICY.md#30-relationship-with-hydra-merlin)).
2. **No link-time edge to a format repository.** Lotus renders what the
   composed stage expresses in standard schemas.
3. **No required neural or vendor-specific dependency.** A neural denoiser
   is an optional `DenoiserBackend`; a vendor SDK is optional, and the
   reference path runs on any Vulkan device with ray query
   ([§16](DESIGN_POLICY.md#16-denoising), [§48](DESIGN_POLICY.md#48-not-done-first)).
4. **The core stays host-neutral.** OpenUSD appears only under
   `adapters/hydra2/`, Vulkan only under `backend/vulkan/`
   ([PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md) §4).

## 5. What this repository does not own

- USD schemas, Hydra, or their versions.
- The MaterialX and OpenPBR specifications.
- Source formats and their material semantics.
- Raster rendering of USD scenes; that is `hydra-merlin`'s role.
- Avatar and toon rendering; that is `hydra-toon`'s role.
- Build, runtime and packaging tooling; that is `open-strata`'s.

## 6. Cross-repository observations

Discrepancies seen in other repositories' documents. They are raised with the
owner, not resolved here.

| Observed | Where | Owner |
| --- | --- | --- |
| — | Nothing recorded yet. | — |
