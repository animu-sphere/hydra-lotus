# Render-pass selection

Measured on 2026-10-05 in the working tree based on `92fbbce`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was an
NVIDIA RTX A5000 (driver 597.16). Hydra used OpenUSD 26.08 lookdev and
CPython 3.13. All runs were local; hosted CI was not measured.

## Scope

This covers render-pass selection, one of the items
[Renderer Phase 1](../roadmap/README.md#status-at-a-glance)
needs alongside its vertical slice: a Hydra render pass traces only the
meshes its collection and render tags select
([scene reference](../reference/SCENE.md#render-pass-selection)). Collection
root and exclude paths and render tags are covered; the collection's
material tag is deliberately ignored. Only the Hydra adapter changed; the
core and the backend did not.

The checks are exact comparisons of mesh sets and counts on a synthetic
stage; sample count and RNG seed do not apply.

## Design

An excluded mesh is hidden for the pass, not removed. The pass's snapshot is
the CPU snapshot with the excluded meshes' `visible` cleared, so the update
plan keeps their geometry resident, as for authored invisibility, and a
selection change rewrites only the instances. The alternative, dropping
excluded meshes from the snapshot, would release and re-upload geometry and
BLASes each time usdview toggles a purpose.

The selection is an exclusion set over the render index's rprims. A mesh
synced outside any render index, as `lotus-renderer-hydra-aov` and
`lotus-renderer-hydra-mesh` do, is therefore traced; for rprims a render
index owns, including and excluding are the same.

The render pass caches the set until the collection, the render tags, the
change tracker's rprim index version or its render tag version changes. The
adapter caches the hidden copy of the scene until the CPU scene or the set
changes, and makes none when the set hides no visible mesh, so an unchanged
frame plans no GPU work.

## Fixtures

**Hydra.** The new `lotus-renderer-hydra-render-pass` test composes a USD
stage in memory with five one-triangle meshes: `/World/Geometry` with the
default purpose, `/World/Proxy` with `proxy`, `/World/Guide` with `guide`,
`/World/Excluded/Inside` and `/Outside`. It syncs the stage through
UsdImaging's scene indices into the render index and executes one Lotus
render pass through `HdEngine`, collection root `/World`, exclude path
`/World/Excluded`, without AOV bindings. After each step the meshes visible
in `GetSelectedSnapshot` must be exactly the expected set, and its triangle
count must equal the set's size:

| Step | Render tags | Traced |
| --- | --- | --- |
| geometry tag | geometry | Geometry |
| proxy tag | geometry, proxy | Geometry, Proxy |
| proxy tag removed | geometry | Geometry |
| unchanged | geometry | Geometry |
| purpose edit: `Proxy` to `default` | geometry | Geometry, Proxy |
| invisible mesh: `Geometry` | geometry | Proxy |
| whole-stage collection | geometry | Proxy, Inside, Outside |
| guide tag | geometry, guide | Proxy, Guide, Inside, Outside |
| guide tag removed | geometry | Proxy, Inside, Outside |
| no render tags | (none) | Proxy, Guide, Inside, Outside |

The first step also requires `/Outside` to be in the CPU scene, so the
collection, not Hydra's sync, is what leaves it out. When the proxy tag is
removed the proxy mesh must keep its geometry buffer, and the unchanged
frame must reuse the previous selected scene pointer.

`lotus-renderer-hydra-render-pass-gpu` runs the same walk with an 8×8 float
colour AOV, so every step creates or uses the GPU renderer. The GPU scene
must then hold one instance per traced mesh, and removing the proxy tag
must not upload geometry. It is labelled `gpu` and returns CTest's skip
code when no Vulkan device is available.

**Hydra behaviour measured on the way.** Hydra syncs only rprims whose
render tag some task requests: with no tags, `/World/Guide` was never synced
until a task requested `guide`, so the walk syncs it first. Both a USD
purpose edit and, in a discarded experiment, a scene delegate marking
`DirtyRenderTag` advanced the rprim index version as well as the render tag
version: with scene-index emulation, OpenUSD 26.08 reinserts an rprim whose
render tag changes.

Four deliberate faults were each reverted before the runs below:

- ignoring the render tags failed `lotus-renderer-hydra-render-pass`
  ("proxy tag removed: traced /World/Geometry /World/Proxy, expected
  /World/Geometry");
- ignoring the exclude paths failed it at the first step ("geometry tag:
  traced /World/Excluded/Inside /World/Geometry");
- removing excluded meshes from the snapshot instead of hiding them failed
  both tests;
- not tracking the render tag version passed both tests, because of the
  reinsertion above. The check stays, since the change tracker's contract
  does not promise a reinsertion.

`lotus-renderer-hydra-aov` and `lotus-renderer-hydra-mesh`, which sync their
meshes outside a render index, passed under every fault.

## Verification

Before the builds, `ninja -t deps` in the Hydra tree listed no object
without recorded header dependencies, the known Japanese-MSVC issue
([roadmap](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)).

- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **17/17 passed**, including
  both new tests and the usdview host test.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**; the usdview host evidence reports zero Vulkan validation
  messages.

The core, ci-core and viewport configurations compile none of the changed
files and were not rerun. The usdview smoke scene has no proxy or guide
geometry, so usdview's purpose toggles were not exercised in the viewer
itself.

The current behaviour and its limits are in the
[scene reference](../reference/SCENE.md#render-pass-selection); remaining
work is in the
[roadmap](../roadmap/README.md#status-at-a-glance).
