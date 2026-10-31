# Skinned meshes

## Evaluation brief

LOTUS-SKEL-01, the last task of the
[Materials & Lighting milestone](../roadmap/README.md#v080--materials--lighting).
The [asset gap survey](2026-10-29-asset-gap-survey.md#results) found every mesh
bound to a `UsdSkel` skeleton rejected with "face vertex index is out of range":
ElephantWithMonochord lost 2 of 3 meshes and CarbonFrameBike lost 4 tubes.

Adopt a fix if the cause is established, a committed fixture's extracted points
match OpenUSD's skinning, a time change updates them through the existing
deformation path, and both assets render their skinned meshes.

## Environment

Measured 2026-10-31 on top of `63aa0ac`: Windows 11 x86_64, NVIDIA RTX A5000 and
the OpenUSD 26.08 lookdev runtime with Python 3.13.

```sh
ctest --test-dir <hydra tree> -R lotus-renderer-hydra-skel --output-on-failure
usdrecord --renderer Lotus --imageWidth 640 [--camera <path>] [--frames <t>] <stage> <name>.png
```

## Cause

A probe through both of UsdImaging's front ends read what the delegate receives
for a skinned mesh:

| Front end | `Get(points)` | `normals` primvar | Ext-computation primvars | Lotus before this task |
| --- | --- | --- | --- | --- |
| Scene indices (`usdrecord`, usdview) | Empty | Removed | `points`, from `skinningPointsComputation.skinnedPoints` | Rejected: every face index is out of range of no points |
| `UsdImagingDelegate` (the viewport) | The rest points | Removed | The same | Drawn at the rest points, without a warning |

The second row is a defect the survey could not see, since it rendered only
through `usdrecord`. Running the computations needs more than the delegate calls:
`HdExtComputationUtils` looks each computation up as a sprim, so a render delegate
without `extComputation` among its sprim types gets a coding error and a crash.

## Implementation

The delegate supports `extComputation` sprims with the plain `HdExtComputation`.
When a mesh's points or primvars are dirty, the mesh runs the computations that
output `points` and `normals` through `HdExtComputationUtils`, on the CPU during
sync, and their results replace what the primvars gave. Everything after that is
the path of any deforming mesh, including per-face parts. The behaviour is
[described in the scene reference](../reference/SCENE.md#hydra-extraction).

## Fixture

`validation/skel/skinned-v1.usda` binds a strip of four quads to three joints in
every way the survey's assets and OpenUSD's computation distinguish. The rig, the
skeleton and one mesh have transforms of their own, and the rig's is animated, so
a point in the wrong space misses.

| Mesh | Case |
| --- | --- |
| Linear | Weighted influences, a bind transform, its own transform |
| Dual | `dualQuaternion` skinning |
| Rigid | One joint for the whole mesh, constant interpolation |
| Smooth | No authored normals, `catmullClark` |
| FaceVarying | Face-varying authored normals |
| Blend | A blend shape with animated weights, then skinning |
| Parts | Two bound `GeomSubset`s, so two scene meshes |
| Rest | A skeleton without an animation, as on three of the bike's tubes |
| Malformed | Influences that do not match the points |
| Plain, Fixed | Controls: time-sampled points without a skeleton, and nothing varying |

## Results

`lotus-renderer-hydra-skel` syncs the fixture through both front ends at times 1,
2, 3, 2.5 and 1, and computes each result independently with `UsdSkelCache` and
`UsdSkelBlendShapeQuery`.

| Check | Result |
| --- | --- |
| World points of every skinned mesh and part against UsdSkel | Within 2e-4, the tolerance for float skinning, through both front ends at every time |
| Malformed | OpenUSD warns and passes the points on as if in skeleton space; Lotus keeps them |
| A time change | Replaces posed geometry and keeps `Fixed`'s; `Plain` follows its samples |
| A second sync at the same time | The scene revision does not change |
| The skeletons' guide meshes | Not in the scene: their render tag is excluded |
| Normals by default | None handed over, so authored normals are gone; `Smooth` matches an independent computation from its posed points |
| Normals with `USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS` (`-computed-normals`) | Vertex and face-varying corner normals match `UsdSkelSkinNormals` and `UsdSkelSkinFaceVaryingNormals` within 2e-3 |

`lotus-renderer-hydra-skel-gpu` renders the fixture at times 1 and 3 through both
front ends. The time change builds no BLAS, refits BLASes and updates the TLAS, and
keeps 12 resident geometries. The pixel where the rigid mesh's posed corner lands
has alpha 0 at rest and above 0.5 posed.

## Fault injection

Each fault was written into the adapter and the three tests rerun.

| Fault | Caught by |
| --- | --- |
| Computed points ignored | All three, under scene indices |
| Computed only at the first sync | All three, at the first time change |
| A computed `points` not applied | All three |
| Rest points win when the primvar answers | All three, under `UsdImagingDelegate` only |
| Computed normals dropped | `-computed-normals` |
| Computed normals' interpolation lost | `-computed-normals`, on FaceVarying |

## Survey assets

| Stage | Result | Against Storm |
| --- | --- | --- |
| ElephantWithMonochord | No rejected mesh. The elephant and the vibrating string render, and frames 0 and 1000 show different poses | The same poses and silhouette at frame 0 |
| CarbonFrameBike | No rejected mesh; the 4 tubes are back | The same cables |

Both meshes of the elephant are authored at about 100 times their `SkelRoot`s'
extents, which `usdrecord`'s free camera frames, so it sits inside the meshes and
**Storm and Lotus both render an empty image**. The survey's "renders empty" had
this second cause. The renders above used a scratch camera on a local wrapper
stage. The assets author vertex normals that UsdImaging withholds, and their scheme
is `none`, so both renderers shade them faceted. The elephant also ran through the
viewport at time 1000 for 8 frames, validation enabled, without a warning. These
renders stay local, since the assets are not redistributable.

## Other findings

`lotus-viewport-hydra`, `-interaction` and `-camera` fail intermittently with
`vkWaitForFences` device loss and nvlddmkm event 153, the
[known fault at frame 2](2026-10-18-viewport-device-loss.md#answer). This task does
not cause it: over 8 rounds of the three tests, 5 of 24 runs failed with the adapter
at `63aa0ac` and 2 of 24 with this change, with the same error. It remains open, and
it bears on the release gate's viewport evidence.

## Decision

Adopt. The cause is established, the fixture matches UsdSkel through both front
ends, a time change refits through the existing deformation path, and both assets
show their skinned meshes.

## Deferred

- GPU skinning. Skinning runs on the CPU during sync, once per mesh per dirty
  sync, and no asset here gives a reason to measure it. OpenUSD's GLSL kernels are
  not used.
- Enabling computed normals by default. `USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS`
  is OpenUSD's setting, off by default; Lotus honours it and matches Storm without it.

## Not covered

- Skinned meshes under an instancer or in a native instance prototype: no surveyed
  asset has them.
- Curves and points, since Lotus has only mesh rprims.
- Motion blur: only the current time is computed.
- Skinning cost on large meshes. The largest skinned mesh here has 1,312 points.
- Hosted CI GPU paths: as with other GPU tests, `-gpu` runs where a device exists.
