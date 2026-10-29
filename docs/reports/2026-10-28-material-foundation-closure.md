# Material foundation closure: LOTUS-MATERIAL-01

> Later: third-party assets demonstrated the need for `UsdTransform2d`, per-face materials and unbound `displayOpacity`, selected as LOTUS-MATERIAL-02 ([asset gap survey](2026-10-29-asset-gap-survey.md#decision)).

Reviewed 2026-10-28 against `b4e2711`. This review adds no measurement and
changes no renderer, shader, translator or harness code. It closes
LOTUS-MATERIAL-01, the Renderer Phase 8 subset of
[workstream C](../design/ROADMAP_POLICY.md#practical-foundation-workstreams),
from the evidence of its five slices. Each item gets one disposition, as
[foundation closure](../contributing/documentation.md#status-and-evidence)
requires: implemented, deferred to a named phase, or an accepted limitation
with its rationale. Closing the subset does not complete Renderer Phase 8.

## Decision

LOTUS-MATERIAL-01 is closed. Every item of the workstream's audit order
has a committed `UsdPreviewSurface` fixture with a C++ mirror, reference
images and Hydra/headless agreement. The audit found no rendering defect.
It found three silent or unstated behaviours. Two now warn: untranslated
connections and `GeomSubset` material bindings. The third, documented, is
the frame of a constant tangent normal. None of the candidate fixes is
needed by an asset at hand, so they are deferred to Renderer Phase 8
instead of being opened in this subset. Expansion starts from demonstrated
asset gaps, as the
[Renderer Phase 8 policy](../design/ROADMAP_POLICY.md#renderer-phase-8--production-material-support)
and the [strategy's entry conditions](../design/POST_V060_STRATEGY.md#5-entry-conditions-for-later-work)
require.

## Exit evidence

| Workstream C exit evidence | Delivered |
| --- | --- |
| Representative USD fixtures and reference images | Five committed fixtures in `validation/materials/` with nine generated images, written by `scripts/material_fixtures.py`, whose `check` command and unit test reject stale files; a 4096-spp mean and per-sample variance per fixture in `validation/reference/` ([contract](../reference/SCENE.md#material-fixtures)) |
| Hydra/headless consistency | UsdImaging's snapshot of every fixture equals its mirror exactly, decoded texels and Storm's computed normals included. Every Hydra image is byte-identical to the backend's image of the mirror, under NEE/MIS and BSDF sampling |
| Documented unsupported parameters | The [translator](../reference/SCENE.md#materials) and [extraction](../reference/SCENE.md#hydra-extraction) contracts list every input that is not read, warns or takes its default; the slices added the roughness-0 floor, the constant normal's frame, the float emission map's light-table bound, the light table's disregard of coverage and the unread `displayOpacity` |
| Profiling and regression coverage for new shader paths | The subset added no shader path, so there is nothing new to profile. Regression coverage: `renderer.material.fixtures` in every headless post-build report compares both integrators and both light-sampling modes with the references and requires each fixture's negative control to be rejected; CTest `lotus-renderer-hydra-material-fixtures` and its `-gpu` variant check Hydra |

## Audit items

| Item | Fixture and evidence | Disposition |
| --- | --- | --- |
| Base colour | `material-basics-v1` ([decision](2026-10-26-material-basics.md#decision)) | Implemented: no fix needed |
| Metallic/roughness | `material-basics-v1` | Implemented: no fix needed; the roughness-0 alpha floor of 10⁻³ is documented |
| Textures/UV | `material-textures-v1` ([decision](2026-10-27-material-textures.md#decision)) | Implemented: untranslated connections, silent before, now warn |
| Shading normals, tangents, normal maps | `material-normals-v1` ([decision](2026-10-27-material-normals.md#decision)) | Implemented: no fix needed; a constant tangent normal's frame is documented |
| Emission | `material-emission-v1` ([decision](2026-10-27-material-emission.md#decision)) | Implemented: ignored `GeomSubset` material bindings, silent before, now warn; the float map bound is documented |
| Alpha masks | `material-alpha-v1` ([decision](2026-10-27-material-alpha.md#decision)) | Implemented: no fix needed; coverage in the light table and `displayOpacity` are documented |

## Deferred and accepted items

| Item | Source | Disposition |
| --- | --- | --- |
| Translate `UsdTransform2d` | [Textures](2026-10-27-material-textures.md#next-items) | Deferred to Renderer Phase 8: needs a per-material coordinate transform in the IR, the material table and the shader; warns until then |
| Name the mesh's `st` for a constant tangent normal | [Normals](2026-10-27-material-normals.md#next-items) | Deferred to Renderer Phase 8: constant non-identity normals are rare in assets |
| Per-face materials from `GeomSubset` bindings | [Emission](2026-10-27-material-emission.md#next-items) | Deferred to Renderer Phase 8: needs mesh splitting or a per-triangle material; warns until then |
| Read `displayOpacity` on unbound meshes | [Alpha](2026-10-27-material-alpha.md#next-items) | Deferred to Renderer Phase 8: documented as not read |
| Path classification | [Upper bound](2026-10-11-wavefront-camera-resolve.md#path-classification) | Deferred to Renderer Phase 8, as assigned at the Renderer Phase 2 exit: the fixtures add no distinct shading class, and the measured benefit remains 1–2% |
| Light-table weight of float emission maps and of covered emitters | [Emission](2026-10-27-material-emission.md#next-items), [alpha](2026-10-27-material-alpha.md#audit) | Deferred to Renderer Phase 9, with light selection: both cost variance only, and the NEE/MIS images match the references |
| Mipmaps | [Textures](2026-10-27-material-textures.md#next-items) | Accepted limitation: minification without mipmaps is unbiased, with 17% more per-sample variance on the minified card; the interactive aliasing mipmaps would remove is unmeasured |
| `opacityMode = transparent`, alpha blending, glass | [Alpha](2026-10-27-material-alpha.md#next-items) | Accepted limitation: coverage opacity is not full transparency, and the [workstream policy](../design/ROADMAP_POLICY.md#practical-foundation-workstreams) requires an explicit transport, sorting and stochastic-transparency model before any such support |
| OpenPBR, MaterialX, glTF PBR mapping, subsurface | Workstream C scope | Outside this subset: Renderer Phase 8 proper |

## Not covered

- Third-party production assets were not audited; the fixtures are authored
  in this repository to isolate one behaviour per object.
- The viewport, usdview and the installed Hydra host were not run with the
  fixtures, and depth and surface AOVs of the fixtures were not rendered.
- References reproduce bit for bit only on the measured device, driver and
  build; on other hardware the comparison is statistical.
