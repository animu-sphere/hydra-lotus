# Material IR v2

## Evaluation brief

LOTUS-MATERIALX-01 implements the accepted
[material IR v2 design](../design/MATERIAL_IR_V2.md) with one model,
`UsdPreviewSurface`. Adopt it if every caller compiles against the tagged
`Material`, the GPU table uses the 128-byte record and binding table and
round-trips every material fixture, and every committed reference image and
material fixture is unchanged
([acceptance](../design/MATERIAL_IR_V2.md#9-acceptance-for-lotus-materialx-01)).

## What changed

- **CPU.** `Lotus::Material` is a `MaterialGeometry` (texture coordinates,
  coverage, base normal, colour set) and a
  `std::variant<PreviewSurface, GltfPbr, StandardSurface, OpenPbr>`.
  `PreviewSurface` keeps today's field names; the other three are empty
  tags, which `RenderWorld` and the GPU scene reject. Texture-input slots
  put opacity (0) and normal (1) first in every model, then the preview's
  base colour, roughness, metallic, emission and specular colour (2–6).
- **GPU.** `GpuMaterialRecord` is 128 bytes: a 16-byte header (model and
  flags, binding base, 64-bit textured mask), 32 bytes of geometry and 80
  of parameters. The 96-byte lookups follow the records in the same buffer
  as a binding table, one per textured slot; `PathConstants` gains its
  address. The readback decodes both back into a `Material`.
- **Shaders.** `ReconstructHit` and `Emission` switch on the model, and
  `EvaluatePreview` is the previous evaluation with its inputs read through
  `MaterialInput` (popcount into the binding table). The any-hit coverage
  test reads only the geometry and slot 0. `SurfaceHit` gains the surface
  summary (`albedo`, `specular_roughness`, `specular_share`), filled with the
  previous formulas, which the temporal pass now reads.
- **Benchmarks.** The fixed benchmarks' JSON records `scene.materials` and
  `scene.material_bytes`.

## Verification

On an NVIDIA RTX A5000 (Windows, MSVC 19.51, Release):

- **References.** `lotus-headless --write-reference` and
  `--write-material-reference` regenerated the Cornell reference and all
  ten material fixtures (`alpha`, `basics`, `emission`, `mtlx`, `normals`,
  `primvars`, `subsets`, `textures`, `transforms`, `transmission`), mean and
  variance: all 22 files are byte-identical to the committed ones. No
  rounding difference arose.
- **Core tree.** 36 tests; 35 passed in the parallel run, and
  `lotus-renderer-restir-gpu` timed out at 300 s while unrelated GPU tests
  ran on the machine. Rerun alone it passed in 307 s (earlier parallel runs
  took up to 295 s).
- **Hydra tree** (`--profile lookdev --intent hydra`): 68 of 68 passed,
  among them the material, texture and material-fixture tests with and
  without the GPU, which check extraction against each fixture's mirror,
  the GPU readback and both integrators.

### GPU material memory of the fixed benchmarks

Before is the previous table, 784 bytes per entry (the old
`GpuSceneStats::material_bytes`); after is the measured
`scene.material_bytes`, records and bindings.

| Workload | Materials | Before (B) | After (B) |
| --- | ---: | ---: | ---: |
| `cornell-v1` | 6 | 4,704 | 768 |
| `textured-v1` | 17 | 13,328 | 3,712 |
| `instanced-v1` | 1 | 784 | 128 |
| `geometry-v1` | 1 | 784 | 128 |
| `lights-v1` | 5 | 3,920 | 640 |
| `environment-v1` | 3 | 2,352 | 384 |
| `many-lights-v1` | 5 | 3,920 | 640 |

`textured-v1`'s 16 textured materials read one binding each (1,536 bytes)
besides their records.

## Decision

Adopt. The acceptance's five conditions hold: the tagged CPU form compiles
across every caller, the record and binding table round-trip the fixtures,
the references are byte-identical, the memory is recorded above, and the
[scene reference](../reference/SCENE.md#materials-and-environment)
documents the layout. LOTUS-MATERIALX-02 (the BSDF API contract) is next.
