# Scene update plan and GPU scene upload

Measured on 2026-10-05 in `hydra-lotus`, on the working tree based on
`ea442a5`, Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release.
The GPU was an NVIDIA RTX A5000 (Vulkan device API 1.4.329, loader API
1.4.321). Hydra used the installed OpenUSD 26.08 lookdev runtime and
CPython 3.13. These are local checks; no hosted CI run was measured.

## Scope

This is the second step of the Renderer Phase 1 vertical slice:
`LotusScene` to `GpuScene`. The core's `SceneExtraction` turns successive
snapshots into update plans, and the Vulkan backend applies them to
device-local geometry and instance buffers
([scene reference](../reference/SCENE.md#update-plan)). No pass reads these
buffers yet. The bootstrap triangle is still what is drawn, so no image in
this run is evidence that scene geometry was rendered. BLAS/TLAS, ray
queries and instancer expansion are not part of it.

All checks are exact comparisons on synthetic scenes. Sample count and RNG
seed do not apply.

## Fixtures

`lotus-renderer-scene-update` is a CPU test without OpenUSD or Vulkan. It
checks key-ordered uploads, the absence of empty geometry, empty plans for
unchanged and camera-only commits, transform and visibility edits that
rewrite instances but upload nothing, a point edit that releases one buffer
and uploads one, removal of a hidden mesh that changes no instance, and a
`Reset` that re-uploads the resident scene without releases.

`renderer.scene.upload`, written by `lotus-headless`, drives a quad, a
triangle and a mesh without triangles through the renderer's GPU scene:
insertion, an unchanged commit, a transform edit, a hide, a point edit and
removal. After each step it reads the device buffers back and compares the
positions, triangles, instance transforms and geometry slots with the CPU
scene. It also checks the lifetime counters: the unchanged commit records
no submission, the transform edit uploads no geometry, the hide keeps both
geometries resident, the point edit reuses the released slot, and removal
leaves no geometry bytes. The check runs before the 1,000 bootstrap
frames, so `renderer.validation.messages` covers the uploads too.

A deliberate fault, XOR-ing each instance's geometry slot with 1 in the
backend, made `renderer.scene.upload` fail with "insertion: device buffers
differ from the scene". The fault was reverted before the runs below.

`lotus-renderer-hydra-aov` now also checks the GPU scene after each pass:
one resident geometry and one instance for the triangle, a replaced buffer
after the topology change to a quad, and no instance but a resident
geometry once the mesh is hidden. The usdview smoke test reads the same
counts from its evidence log: one resident geometry and one instance on the
first frame and after the point edit, which uploads exactly one more
buffer.

## Verification

Before the builds, objects with no recorded header dependencies were
deleted, the known Japanese-MSVC workaround
([roadmap](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)); the Hydra build
recompiled every object.

- `ost build --jobs auto`, `ost test`: **8/8 passed**. `ost validate`:
  **passed**, `renderer.scene.upload` OK, Hydra checks skipped by
  configuration.
- `ost build --without-runtime --intent ci-core`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**; the scene
  plan test runs, GPU evidence is an explained SKIP.
- `ost build --profile lookdev --intent hydra`,
  `ost test --profile lookdev --intent hydra`: **14/14 passed**, including
  the AOV integration and usdview host tests.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**; every renderer assertion including `renderer.scene.upload`
  OK, zero Vulkan validation messages. The viewport and artifact-integrity
  checks were SKIP because this run launched no viewport and packaged no
  artifact.

The current API and its limits are in the
[scene reference](../reference/SCENE.md#gpu-scene); remaining work is in the
[roadmap](../roadmap/README.md#status-at-a-glance).
