# Tangent-space normal inputs and normal maps

Measured on 2026-10-08 in the working tree based on `adb9442`, on Windows
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was
an NVIDIA RTX A5000, Vulkan API 1.4.329. Hydra used OpenUSD 26.08 lookdev
and Python 3.13. All runs were local; hosted CI was not measured.

## Scope

The next slice of
[Renderer Phase 1.5](../roadmap/README.md#status-at-a-glance):
`UsdPreviewSurface.normal`, as a signed tangent-space constant or an RGB
`UsdUVTexture` lookup. The existing image decoding, texture residency,
corner UV sets and shading-normal diagnostic serve this slice. No target,
dependency, pipeline or pass is added.

## Implementation

`Material::normal` defaults to `(0, 0, 1)` and `normal_texture` optionally
replaces it. The translator accepts finite signed constants clamped to
[-1, 1], and reuses texture translation for RGB, scale, bias, fallback,
wrap and colour space. It performs no implicit unsigned-to-signed texture
conversion. The normal lookup follows the existing inputs in translation
order, preserving their choice of the material's single UV source.

The GPU material record is 384 bytes: the former constants, a float4 for
the tangent normal, and five 64-byte texture inputs. Upload validation,
texture resolution and readback include the new input. A non-identity
constant normal also selects its named UV set without any texture lookup.

At each hit the shader computes the world-space UV Jacobian from triangle
edges and the selected corner UVs, projects increasing-s tangent onto the
mesh shading normal's plane and preserves the increasing-t handedness.
Absent, degenerate or ill-conditioned UVs use the existing Duff frame.
The mapped normal is normalized and oriented to the geometric side; zero
or non-finite vectors and normals facing away from the ray leave the mesh
normal. The identity follows the previous path exactly. Geometric normals
still bound reflected directions and offset ray origins.

This is a per-hit triangle frame, with no stored vertex tangents or
MikkTSpace compatibility guarantee. The current contract belongs in the
[scene reference](../reference/SCENE.md#path-tracing).

## Verification

**Core and GPU scene.** The core rejects out-of-range or non-finite normal
constants and scalar channels on normal lookups. The update-plan test
checks shared normal-texture resolution, texture replacement and removal,
and UV selection for a constant normal without texture inputs. The
headless scene-upload walk reads back both the constant and the fifth
lookup, including its resident texture slot, scale and bias.

**Normal diagnostic.** `renderer.path.normal_maps` traces pixel centres of
eight panels at 64x64. A separate CPU oracle intersects world-space
triangles and solves the UV Jacobian in double precision, dividing by its
determinant rather than using the shader's rescaled differential vectors.
It combines this with the existing independent bilinear-filter oracle.
Pixels within 0.02 barycentric units of an edge are skipped; every panel
must contribute at least 40 checked pixels, with finite components within
0.012 of the oracle and alpha exactly 1.

The panels cover regular, mirrored and rotated UVs; signed float and raw
8-bit images; interpolated mesh normals under a mirrored, sheared,
non-uniform transform and a rotation; a backface; degenerate UVs; an absent
selected set in a geometry that has another set; and a missing image's
unscaled fallback. The same oracle checks texture replacement, zero-map
fallback, a signed constant normal and a tangent-only normal fallback.
The diagnostic is deterministic and samples pixel centres, with no
stochastic averaging.

**Transport.** A white near-mirror plane sees a black environment with an
emissive wall outside the camera view. At 4 spp, first sample index 0,
the unperturbed mirror is black; a float normal map tilted 30 degrees
towards the wall gives exactly `(2, 1, 0.5)`, within the existing exact
radiance tolerance. This proves normal maps affect BSDF sampling as well
as the diagnostic.

**Hydra.** The existing `lotus-renderer-hydra-texture` and `-gpu` tests
include signed constants, non-finite constants, RGB lookup translation,
shared image requests and rejection of a scalar normal connection. A
stage uses a raw PNG normal map with scale 2 and bias -1 on a white mirror.
At 1 spp, first sample index 0, all 64 pixels of its 8x8 colour AOV must
show the emissive wall `(2, 1, 0.5)` within 1e-4. Mirroring s turns the
mirror away from the wall and produces the white fallback environment.
Changing the normal-map file to one tilted the other way, setting the
lookup result to zero, and disconnecting the normal input also produce
white. Snapshots check geometry replacement, decoded-image reuse,
replacement and final release at the same steps.

The identity-camera Hydra scene views the plane from negative Z; its wall
is on that side. The headless orthographic scene views from positive Z,
with its wall on the positive side. This also exercises two-sided shading.

**Reference regression.** The Cornell box still passes at 1, 16, 64, 256
and 1024 spp. The reference sample sequence reproduces the committed
reference bit for bit; the reference files were not regenerated.

## Fault injections

Temporary shader copies were compiled and placed in the core test
executable's shader directory, then the original SPIR-V was restored in a
`finally` block. Repository shader sources were never changed for these
injections. Each headless run exited 1 and reported
`renderer.path.normal_maps` as fail:

| Fault | Detected result |
| --- | --- |
| Skip normal-map evaluation | At pixel (1, 4), normal x is 0 instead of -0.418820 |
| Ignore the UV determinant's sign | At pixel (17, 4), normal x is 0.425624 instead of -0.425623 |

## Commands and results

- `ost build --jobs auto`, `ost test`: **8/8 passed**;
  `ost validate --strict-renderer-evidence`: **passed**.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**,
  with explained GPU SKIPs.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **23/23 passed**, including
  normal-map CPU/GPU coverage and the usdview host test;
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**.

The core and Hydra reports include `renderer.path.normal_maps`,
`renderer.scene.upload`, `renderer.path.reference` and
`renderer.validation.messages` as **pass**, with zero Vulkan validation
messages. `ninja -t deps` was checked in the Hydra tree before the build;
no objects with zero recorded dependencies needed removal.
