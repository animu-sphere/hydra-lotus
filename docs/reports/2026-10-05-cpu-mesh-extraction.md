# CPU mesh extraction and snapshot lifetime

Measured on 2026-10-05 in `hydra-lotus`, on the working tree based on
`eb69bd9`, Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release.
Hydra used the installed OpenUSD 26.08 lookdev runtime, CPython 3.13 and
NVIDIA RTX A5000. These are local checks; no hosted CI run was measured.

## Scope and fixtures

This is the first CPU step of Renderer Phase 1. The adapter now stores
coarse mesh geometry and one ordinary placement per mesh in `LotusScene`.
GPU scene upload, instancer expansion, BLAS/TLAS and ray tracing are not
part of this measurement. The display still uses the fixed bootstrap
triangle; its shape is not evidence that the extracted mesh was rendered.

`lotus-renderer-scene-mesh` uses two named synthetic triangle meshes to
check retained snapshots, insertion, unchanged commits, point edits,
transform/visibility updates, removal and invalid input. Geometry buffers
are shared across placement changes, while prior snapshots retain their
original points, transforms and mesh membership. Multiple visible mesh
triangles remain distinct from the single bootstrap draw.

`lotus-renderer-hydra-mesh` uses a synthetic quad and a triangle marked as a
hole. It checks fan triangulation, authored-face mapping, left/right winding,
hole removal/restoration, matrix conversion with rotation and translation,
point edits, visibility and destruction. Delegate read counters verify that
clean meshes fetch nothing and that transform/visibility updates do not read
points or topology. Invalid indices, negative face counts and non-finite
positions remove old geometry; valid input recovers it.

`lotus-renderer-hydra-aov` also switches its fixture from one triangle to a
quad. Its CPU scene has two triangles, while the bootstrap backend still
converges its colour/depth output. The existing clear/preservation tests
continue to run. The installed usdview smoke scene still checks a first
frame and a point edit through the real host.

The mesh checks are CPU assertions, so spp and RNG seeds are not applicable.
The headless GPU evidence is the existing deterministic 1,000-frame
bootstrap workload, not a path-traced reference image or performance claim.

## Verification

Both the runtime-free `ci-core` and lookdev `hydra` intents were built with
`ost build`, using the installed Visual Studio Ninja explicitly. Before the
first build of each intent, its build outputs were cleaned with Ninja's
`-t clean` to avoid the known Japanese-MSVC header-dependency issue. No
compiler or project setting was changed for that workaround.

- `ost test --without-runtime --intent ci-core`: **7/7 passed**. The new
  scene test runs without OpenUSD or Vulkan; GPU evidence is an explained
  SKIP in this configuration.
- `ost test --profile lookdev --intent hydra`: **13/13 passed**, including
  mesh extraction, multi-triangle AOV compatibility and usdview host checks.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**. Completion-bound GPU, AOV, plugin, render-buffer and host
  evidence passed; Vulkan validation messages were zero. The viewport and
  artifact-integrity checks were SKIP because this run did not launch a
  viewport or package an artifact.

The current API and its limits are in the
[scene reference](../reference/SCENE.md); remaining work is in the
[roadmap](../roadmap/current.md#renderer-phase-1--reference-path-tracer).
