# Project layout

The binding structural contract: which targets exist, where new code goes, and
which way dependencies point. Rationale is in
[design/DESIGN_POLICY.md](../design/DESIGN_POLICY.md); what each target can do
today is [reference/CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md).
A structural change updates this page first, in its own pull request.

## 1. What the project is

One CMake project, adopted by OpenStrata as a **renderer project**
(`openstrata.renderer.yaml`), not a plugin workspace. Its internal libraries are
CMake targets, not independently versioned packages. The installed `Lotus`
CMake package and the `hdLotus` Hydra module are the distribution boundary.

It was generated with `ost init --template renderer --name lotus` (template
`renderer` 0.5.4, `ost` 0.23.14; recorded in `openstrata.scaffold.yaml`). From
that moment every file is project-owned; the template is not re-applied.

| Name | Value |
| --- | --- |
| repository | `hydra-lotus` |
| OpenStrata project / renderer name | `lotus` |
| CMake project and package | `Lotus` (`find_package(Lotus)`, `Lotus::` targets) |
| C++ namespace | `Lotus` |
| Public header root | `include/lotus/` |
| Hydra module and renderer plugin | `hdLotus` (display name `Lotus`) |
| CMake option prefix | `LOTUS_` |

## 2. Targets today

| Directory | Target | Alias | Role |
| --- | --- | --- | --- |
| `core/render-world/` | `lotus-render-world` | `Lotus::RenderWorld` | host-neutral camera and CPU mesh scene; immutable snapshots and shared geometry ([scene reference](../reference/SCENE.md)) |
| `core/render-extraction/` | `lotus-render-extraction` | `Lotus::RenderExtraction` | scene changes → GPU update work |
| `backend/vulkan/` | `lotus-render-vulkan` | `Lotus::Vulkan` | Vulkan backend: persistent offscreen colour/depth attachments and clear control, swapchain presentation, Slang shaders |
| `adapters/headless/` | `lotus-headless` | — | headless runner; writes `renderer-report.json` |
| `adapters/viewport/` | `lotus-viewport` | — | standalone GLFW window; optional (`LOTUS_ENABLE_VIEWPORT`) |
| `adapters/hydra2/` | `hdLotus`, `lotus-hydra2-runtime` | — | the `HdRenderDelegate` adapter, coarse mesh extraction into the core scene, and CPU AOV storage/binding validation ([AOV reference](../reference/AOVS.md)); optional (`LOTUS_ENABLE_HYDRA2`) |
| `validation/` | CTest only | — | core boundary, evidence and install-tree checks |

`adapters/hydra2/` is OpenStrata's name for the Hydra scene-input slot; the code
is a classic `HdRenderDelegate` ([DESIGN_POLICY.md](../design/DESIGN_POLICY.md)
§52).

## 3. Where new code goes

The design policy's components map onto this layout as follows. A row is a
placement rule; it does not say the component exists. The right-hand column
names the directory of the implementation direction's sketch
([DESIGN_POLICY.md](../design/DESIGN_POLICY.md) §33) that the row replaces.

| Design policy component | Directory | Target | Direction §33 |
| --- | --- | --- | --- |
| `LotusScene`: geometry, instances, materials, textures, lights, cameras, render settings (§4.1) — the scaffold's `RenderWorld` is its seed | `core/render-world/` | `lotus-render-world` | `src/lotus/scene/` |
| The update plan: which `LotusScene` changes become which GPU uploads, BLAS refits or rebuilds (§4.2, §20) | `core/render-extraction/` | `lotus-render-extraction` | — |
| Lotus material IR (§18) | `core/material/` when it exists | new core target | `src/lotus/material/` |
| Integrator selection, interactive / progressive mode, deterministic-mode settings (§21, §26, §34) | `core/integrator/` when it exists | new core target | `src/lotus/integrator/` |
| Render graph (§35) | `core/render-graph/` when it exists | new core target | — |
| Vulkan device, memory, descriptors, pipelines, GPU timers | `backend/vulkan/` | `lotus-render-vulkan` | `src/vulkan/{device,memory,descriptor,pipeline}/` |
| `GpuScene`, BLAS / TLAS, `TraversalBackend` (§4.2, §6, §20) | `backend/vulkan/` | `lotus-render-vulkan` | `src/lotus/gpu/`, `src/vulkan/raytracing/` |
| Integrator implementations: reference, wavefront, ReSTIR (§7–§14, §34) | `backend/vulkan/` | `lotus-render-vulkan` | `src/lotus/{integrator,sampling,restir}/` |
| Temporal history, `DenoiserBackend` and denoisers (§15–§16) | `backend/vulkan/` | `lotus-render-vulkan` | `src/lotus/denoise/` |
| Slang shaders: `common/`, `bsdf/`, `sampling/`, `wavefront/`, `restir/`, `denoise/` | `backend/vulkan/shaders/<group>/` | — | `shaders/<group>/` |
| Hydra prims, render pass, AOVs, material translator (§3.1, §18, §32) | `adapters/hydra2/src/` | `lotus-hydra2-runtime` | `src/hdLotus/` |
| Headless runner, benchmark, test-scene runner (§3.1) | `adapters/headless/`; a separate executable gets `adapters/<name>/` | `lotus-headless` | `tools/`, `examples/` |
| Golden-image and correctness tests (§26) | `validation/` | CTest | `tests/` |
| Public headers — core (`render_world.hpp`, `extraction.hpp`) and backend (`vulkan_backend.hpp`, `vulkan_present.hpp`) | `include/lotus/` | — | — |

Once `backend/vulkan/` grows, it is split into subdirectories inside the same
target before it is split into targets.

## 4. Dependency directions

```text
adapters/{headless,viewport,hydra2}
        │
        ▼
backend/vulkan ──→ core/render-extraction ──→ core/render-world
```

1. `core/` depends on nothing but the C++ standard library.
2. Public core headers contain no OpenUSD, Hydra, Vulkan, Slang, windowing or
   DCC type. Backend headers share `include/lotus/` but expose no Vulkan type
   either: the backend hands out plain C++ results, and Vulkan stays in its
   `.cpp` files. `validation/check-core-boundary.cmake.in` discovers all public
   `.h`, `.hpp`, `.hh`, `.hxx` and `.inl` files recursively under
   `include/lotus/` at test time, including the backend headers. New headers
   need no manual registration. It rejects foreign dependency tokens; it is
   a source check, not a C++ type-system proof.
3. A backend depends on `core/`, never on an adapter or on another backend.
4. OpenUSD appears only under `adapters/hydra2/`. GLFW appears only under
   `adapters/viewport/`.
5. No target links a sibling renderer or a format repository
   ([integration scope §4](../design/INTEGRATION_SCOPE_POLICY.md#4-dependency-rules)).

## 5. Build intents and runtime profiles

| Build | How | Runtime profile | OpenUSD |
| --- | --- | --- | --- |
| default | `ost build` | `core` (from `openstrata.toml`) | none |
| core CI | `ost build --without-runtime --intent ci-core` | none; Vulkan disabled | none |
| Hydra adapter | `ost build --profile lookdev --intent hydra` | `lookdev` or `usd` (a real runtime) | yes |
| standalone viewport | `ost renderer viewport` | `core` | none |

The `hydra` intent is declared in `openstrata.toml`
(`LOTUS_ENABLE_HYDRA2=ON`). `ost renderer view` requests the same adapter
through `OST_RENDERER_ADAPTERS=hydra2`.

`openstrata.ci.yaml` owns the CI cells and runtime artifact pins. The registered
external workflow `.github/workflows/renderer-ci.yml` consumes their resolved
values through `ost ci matrix`, runs both cells on Windows hosted runners for
PRs, main pushes and manual dispatch, and uploads reports and test logs.
OpenStrata's plugin workspace generator is not used for this renderer
([measured limitations](../reports/2026-10-04-foundation-ci.md)).

## 6. Install tree

```text
<prefix>/
├── bin/lotus-headless, bin/shaders/*.spv
├── include/lotus/
├── lib/lotus-render-*.lib|.a
├── lib/cmake/Lotus/           LotusConfig.cmake, LotusTargets.cmake
├── lib/usd/hdLotus/           hdLotus module, resources/plugInfo.json, shaders/   (Hydra build only)
└── share/lotus/               openstrata.renderer.yaml, openstrata.scaffold.yaml, tests/usdview-smoke.usda
```
