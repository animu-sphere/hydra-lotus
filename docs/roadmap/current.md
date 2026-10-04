# Current

Renderer Phase 0 and the work around it. Which release carries the phase is
the [status table](README.md#status-at-a-glance).

Legend: ✅ done · 🚧 in progress · ⬜ not started · ⛔ blocked · ⚠️ accepted workaround

## Renderer Phase 0 — Bootstrap

The OpenStrata renderer scaffold was generated on 2026-10-04 and passes its
own contract on Windows
([report](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md)).
What is left of
[design policy §36](../design/DESIGN_POLICY.md#36-renderer-phase-0--bootstrap):

- ✅ **Plugin registration, `HdRenderDelegate` and render pass.** `hdLotus`
  is discovered, creates its delegate, and draws a first frame in
  `testusdview`.
- ✅ **Vulkan device.** Device bring-up with validation layers, messages
  treated as errors.
- ✅ **Image output.** The Hydra path renders offscreen at the AOV's
  resolution and copies the products into CPU `HdRenderBuffer`s.
- ✅ **Camera.** The Hydra camera's view and projection, and the framing's
  display and data windows, place the bootstrap triangle; the headless runner
  checks it through a perspective camera.
- ⬜ **A persistent offscreen renderer.** Every Hydra frame creates a Vulkan
  instance, device and pipeline, against
  [§23](../design/DESIGN_POLICY.md#23-cpu-performance). The device and
  pipeline outlive frames; the targets are recreated only when the AOV size
  changes.
- ✅ **Triangle.** The scaffold's hard-coded triangle, in the headless
  runner, the viewport and `testusdview`.
- 🚧 **Basic AOVs.** Colour, depth and primId exist as CPU render buffers.
  - ⬜ Decide the Phase 0 AOV set and its formats with the debug AOVs of
    [§25](../design/DESIGN_POLICY.md#25-debug-and-validation) in mind.

## Before Renderer Phase 1

Principle 8 of [§51](../design/DESIGN_POLICY.md#51-decision-principles) puts
these ahead of the first path tracer:

- ⬜ **Per-pass GPU timestamps**, reported by the headless runner
  ([§24](../design/DESIGN_POLICY.md#24-gpu-profiling)).
- ⬜ **Deterministic mode and a golden-image test** in `validation/`
  ([§26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode)).
- ⬜ **Ray query capability probe.** Report `VK_KHR_acceleration_structure`
  and `VK_KHR_ray_query` in `renderer-report.json` and `SKIP` the
  path-tracing checks with an explanation where they are missing.
- ⬜ **Settle [DES-Q2 and DES-Q3](../design/DESIGN_POLICY.md#53-open-questions)**:
  whether GGX is in the baseline, and what keeps `PathState` open to spectra.

## Project infrastructure

- ⬜ **CI.** A generated OpenStrata CI lane: the `core` build with its GPU
  checks as capability-gated `SKIP`s on hosted runners, and the `hydra`
  intent against a digest-pinned runtime.
- ⬜ **Documentation check.** A `scripts/check_docs.py` that resolves
  relative links and checks category indexes.
- ⬜ **Core boundary check by glob.** The check lists its headers by name
  ([PROJECT_LAYOUT.md §4](../architecture/PROJECT_LAYOUT.md#4-dependency-directions));
  it should find every public core header itself.
- ⚠️ **Objects without header dependencies on a Japanese MSVC host.** In the
  `hydra` and viewport trees, objects record `#deps 0` in Ninja's log, so a
  header edit does not rebuild them. Until OpenStrata or the template
  resolves it, list them with `ninja -t deps` in the build tree and delete
  every `.obj` with `#deps 0` before building
  ([report](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md) §3).
