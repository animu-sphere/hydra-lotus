# Material normals fixture

## Evaluation brief

LOTUS-MATERIAL-01, third slice. The
[first](2026-10-26-material-basics.md) and
[second](2026-10-27-material-textures.md) slices established committed
`UsdPreviewSurface` fixtures with independent C++ mirrors, reference images
and Hydra/headless agreement, and audited base colour, metallic/roughness,
textures and UV. This slice audits the fourth item of the
[workstream's order](../design/ROADMAP_POLICY.md#practical-foundation-workstreams),
shading normals, tangents and normal maps, against the
[shading-normal and normal-map contract](../reference/SCENE.md#path-tracing),
the [Hydra mesh extraction](../reference/SCENE.md#hydra-extraction) and the
[translator](../reference/SCENE.md#materials), on a stage that reaches them
from a USD file.

The new fixture, `material-normals-v1`, is authored in this repository by
`scripts/material_fixtures.py`, with two 8-bit normal maps. Its mirror
computes the meshes as the adapter triangulates them and the computed
normals as Storm's helper does in float, so the Hydra CTest checks the
authored-normal expansion, the smooth-normal fallback and the translator
from file to image.

Adopt the fixture if the stage extracts to the mirror, every rendered mode
matches the reference by the [DES-Q5 metric](../reference/SCENE.md#reference-images),
Hydra's images are the mirror's bit for bit, and the metric rejects a
flipped green channel on the normal map. The shading-normal and tangent
arithmetic is already checked against a CPU oracle by
`renderer.path.normal_maps` ([report](2026-10-08-normal-maps.md)); this
slice checks what real USD assets feed it. Emission and alpha masks are
later slices; the viewport, temporal/SVGF modes and performance are not
covered. This slice adds no shader path.

Conditions: Windows AMD64, MSVC 19.51 Release, NVIDIA RTX A5000, driver
597.16, Vulkan 1.4.329, validation and synchronization validation active,
clocks unlocked. Source `a84c19c` plus this change. The binaries' hashes,
the reference's and fixture files' hashes and every value are in the
[data](2026-10-27-material-normals.json).

## Fixture

`material-normals-v1` is a 5×5 grid of coarse spheres (16 segments, 8
rings, so that faceting shows) and cards under the other fixtures' camera,
dome, rect key light and `displayColor` backdrop, at 160×160. Its rows
cover authored mesh-normal interpolations; meshes without usable normals
and inward normals; normal-map encodings and a constant tangent normal;
tangent frames under mirrored, rotated, mirrored-transform and stretched
mappings and an absent texture-coordinate set; and normal maps on spheres
with longitude–latitude coordinates. The normal map is a 4×4 dome tilted
towards increasing s and t; a second image stores it with green running
down, as DirectX-convention maps do. The
[scene reference](../reference/SCENE.md#material-fixtures) lists every
object.

`usdchecker` reports exactly the three lookups that depart from the
specification's normal-map encoding on purpose: Auto's missing `raw`
colour space, Unscaled's missing scale and bias, and DirectX's negative
green scale. The reference was written with
`lotus-headless --write-material-reference validation/reference`; all
three fixtures took 7.3 seconds, and the rewritten `material-basics-v1`
and `material-textures-v1` references are byte-identical to the committed
ones.

## Audit

| Item | Contract | Evidence | Finding |
| --- | --- | --- | --- |
| Authored interpolations | Vertex, uniform and face-varying normals reach every corner; indexed ones are flattened | Vertex, FaceVarying (indexed) and Uniform extract to the mirror's corners | As documented |
| `primvars:normals` over `normals` | UsdImaging takes the primvar | Primvar's (0, 0, 1) attribute loses to its vertex primvar | As documented |
| Computed normals | Without usable normals, schemes other than `none` and `bilinear` receive Storm's coarse smooth normals | Computed's normals equal the mirror's float reimplementation of `Hd_SmoothNormals` bit for bit | As documented |
| Left-handed orientation | Winding normalized; contributions reversed | LeftHanded, every face reversed, extracts to the same normals; HdMeshUtil starts a left-handed quad's triangles at its third vertex, which the mirror now encodes | As documented |
| Malformed normals | Ignored with a warning; the scheme's fallback | Malformed warns ("a vertex or varying primvar needs one value per point") and is computed | As documented |
| `bilinear` and `none` | Geometric shading without authored normals | Bilinear and Faceted extract without normals | As documented |
| Inward normals | Turned to the geometric normal's side | Inward extracts its inward normals and renders as an outward-shaded sphere | As documented |
| Normal-map encoding | No implicit unsigned-to-signed decoding; `auto` 8-bit RGB is sRGB | Auto decodes as sRGB, which pulls most texels towards −1 and visibly distorts the dome; Unscaled reads [0, 1] as signed; DirectX's authored green flip reproduces Mapped | As documented; assets must author `raw`, scale and bias, which `usdchecker` already enforces |
| Constant tangent normal | A non-identity constant may read the material's set for its frame | Through Hydra a material without lookups names no set, so Constant and SphereConstant take the normal-only frame although their meshes have `st` | **Documented**: the translator's consequence was not stated; see [next items](#next-items) |
| Tangent frames | Tangent along increasing s, bitangent towards increasing t; handedness retained | MirroredS, Rotated, MirroredTransform and Stretched render the dome lit from the same side as Mapped; AbsentSet takes one texel at the normal-only frame | As documented; the frame arithmetic itself is the oracle check's |
| Curved surfaces | Per-hit triangle frames, no stored tangents | Spheres with longitude–latitude `st` over vertex, faceted and computed normals, including a glossy metal | As documented; tile edges of the repeating dome show as creases by design |
| Not covered | MikkTSpace tangents, normal-map mipmaps, `UsdTransform2d` on normal maps, subdivision limit normals | Not authored | Unchanged; already listed as unsupported |

## Results

**Extraction.** UsdImaging's snapshot of the stage equals its mirror
exactly: materials, bindings, triangles, texture-coordinate sets, three
decoded textures (`tilts.png` as `raw` and `auto`, `tilts-dx.png` as
`raw`), lights and environment. No transform, camera, position or normal
set differs even by rounding, computed normals included. The only warning
is Malformed's.

**Images against the reference** (sample index 2²⁰, 8-pixel tiles):

| Mode | spp | Image z (R, G, B) | Max tile \|z\| | Mean tile z² |
| --- | ---: | --- | ---: | ---: |
| Reference integrator, BSDF | 16 | 1.47, 1.42, 1.41 | 3.61 | 1.02 |
| Reference integrator, BSDF | 256 | −0.20, −0.23, −0.24 | 3.09 | 1.00 |
| Reference integrator, NEE/MIS | 16 | 0.10, 0.13, 0.14 | 1.88 | 0.13 |
| Reference integrator, NEE/MIS | 256 | 0.50, 0.54, 0.55 | 1.65 | 0.18 |
| Wavefront integrator, NEE/MIS | 16 | 0.10, 0.13, 0.14 | 1.88 | 0.13 |
| Wavefront integrator, NEE/MIS | 256 | 0.50, 0.54, 0.55 | 1.65 | 0.18 |
| Hydra, NEE/MIS | 256 | 0.50, 0.54, 0.55 | 1.65 | 0.18 |
| Hydra, BSDF | 256 | −0.20, −0.23, −0.24 | 3.09 | 1.00 |

The wavefront images are byte-identical to the reference integrator's, and
each Hydra image is byte-identical to the backend's image of the mirror.
The other two fixtures' rows are unchanged to the last digit.

**Power and reproducibility.** The Mapped material's green read flipped,
which changes five cards, is rejected at max tile |z| 64.46 at 256 spp.
Re-rendering the reference's own samples reproduces the committed mean bit
for bit on this device and build, for every fixture.

**Cost.** The headless report, all checks included, took 48 s; the Hydra
CTests took 0.09 s (CPU) and 45.3 s (GPU, 27.4 s before). The new fixture's
share was not timed separately.

## Decision

Adopted: `material-normals-v1` and its mirror, including the float
reimplementation of Storm's smooth normals and the left-handed quad
triangulation. Authored and computed mesh normals, normal-map encodings and
tangent frames need no renderer or translator fix; the constant normal's
frame through Hydra is now documented.

## Next items

- A constant tangent normal through Hydra always takes the normal-only
  frame. Naming the mesh's `st` for it, as a reader would, is a bounded
  candidate fix; constant non-identity normals are rare in assets, so it
  waits for one that needs it.
- `UsdTransform2d` remains untranslated (from the
  [textures slice](2026-10-27-material-textures.md#next-items)); it would
  move normal maps as well.
- Following the audit order: emission, then alpha masks, each as a fixture
  beside these.

## Unavailable and unmeasured

- The reference is a finite 4096-spp estimate, not ground truth; agreement
  is statistical except where stated byte-identical.
- Bit-for-bit reference reproduction holds on this device, driver and
  build only. The mirror's computed normals equal OpenUSD 26.08's on this
  compiler; another OpenUSD build could round differently.
- The tangent cards' agreement with Mapped is visual; the frame arithmetic
  is checked by `renderer.path.normal_maps`, not by this fixture.
- The viewport, usdview and the installed Hydra host were not run with the
  fixture. `ci-core` was not run; of the Hydra CTests only the two
  material-fixture CTests ran.
- Third-party production assets were not audited.
