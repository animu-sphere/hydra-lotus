# GPU primary-surface temporal history

Measured on 2026-10-12 against `9cc0704` plus LOTUS-TEMPORAL-02, using MSVC,
the OpenStrata core Release and OpenUSD 26.08 lookdev/Hydra configurations on
the Windows workstation with an NVIDIA RTX A5000. The runtime-free build
disables Vulkan. The [temporal reference](../reference/TEMPORAL.md) owns the
product contract; the [roadmap](../roadmap/README.md#status-at-a-glance) owns
phase status.

## Primary-surface checks

`lotus-renderer-temporal-gpu` exercises the opt-in backend entry point under
the reference and wavefront integrators. Every successful frame in its
sequences required zero Vulkan validation messages, including synchronization
validation. The assertions established:

- At 64×48, pixel-centre primary depth is 4 and geometric normal is +Z for
  the stationary plane. All 3,072 pixels have stable nonzero placement and
  material identities, zero motion and previous-depth 4. A 3-sample call
  followed by a 2-sample call advances two temporal frames and preserves
  5-sample progressive transport. The preceding GPU buffer is retained
  byte for byte.
- A camera-eye translation X +0.25 yields backward motion X +2 pixels;
  previous-image clipping invalidates the last two columns. With that
  camera fixed, object translation `(0.5, 0.25, 0)` yields backward motion
  `(-4, 1.5)` in 2,760 pixels with in-frame history and rejects positions
  beyond the preceding image.
- Combined object translation `(0.5, 0.25, 0)` and camera-eye translation
  `(0.25, 0, -1)` matches an independent pinhole projection at all 3,072
  pixels. For current NDC `(nx, ny)`, previous pixels are
  `((0.5 + (3*nx - 0.25)/8)*64, (0.5 - (3*ny - 0.25)/8)*48)`.
  Current depth is 3; expected previous depth stays 4. Motion/depth
  comparisons use an absolute tolerance of 0.0002.
- Inserting meshes/materials before the visible surface shifts GPU table
  slots without changing its temporal identities. Material edits and used
  texture insertion change material identity and clear the unchanged-material
  flag; unrelated texture insertion preserves both. CPU identity checks
  additionally cover texture replacement and mesh removal/reinsertion.
- Duplicate indices, zero-sample calls, empty targets and unuploaded
  snapshots fail without consuming the preceding frame. A converged
  progressive call still advances temporal history without adding samples.
  Primary metadata is generated even when a zero depth clear prevents the
  ordinary camera pass from writing the depth attachment.
- Resizing to 80×60 with display window `(-8, 4, 96, 48)` and data window
  `(5, 7, 60, 40)` resets history, leaves pixels outside the data window
  zero and recovers on the next compatible call. A subsequent cropped
  camera move yields backward X +4 pixels and rejects history outside the
  cropped window. Window edits, explicit cuts, compatibility-key edits and
  frame gaps select their documented reset reasons.
- A plane rotated 60 degrees about Y produces geometric normal
  `(0.8660254, 0, 0.5)`. An orthographic camera with X/Y span 8 and eye
  translation -0.25 produces backward X -3 pixels through the offset display
  window, with expected previous depth 3 and cropped-history clipping.
- Instancing-mode and placement-count changes reject correspondence and
  change the placement incarnation. Removing the entire scene clears every
  surface record while retaining timestamp availability.
- Temporal rendering preserves progressive colour/depth bytes in the
  stationary sequence. A successful ordinary rendering call breaks the
  temporal stream. The existing reference-image and wavefront transport
  regressions also pass.

The CPU `lotus-renderer-temporal` checks the window projection independently:
object point `(1, 0.5, -4)` maps to `(84, 29)` through display window
`(-16, 8, 160, 48)` and data window `(10, 12, 100, 40)` in a 128×64 image.
Its private identity-registry checks also run without Vulkan. Placement
pairing now fetches a single composed instancer transform directly rather
than copying the entire placement list for every queried ordinal.

## GPU time and memory

The final targeted core run measured the stationary primary-surface pass
at 0.015264 ms under reference transport and 0.034080 ms under wavefront
transport. These are individual timestamp samples from a correctness run,
covering the surface-buffer clear and draw, excluding host metadata writes
and readback; they are not a comparative performance benchmark.

At 64×48 with one instance, surface storage is 786,432 bytes: two 196,608-byte
device buffers and a 393,216-byte host readback mirror. Metadata capacity is
320 bytes: 224 for cameras/windows plus one 96-byte instance record.
Resources persist at a fixed size; resize replaces surface storage. Instance
metadata capacity grows when needed. The pipelines and timestamp pool are
created at renderer initialization.

## Commands and regression evidence

Commands run from the repository root:

```sh
ost build --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core -R 'temporal|core-boundary' --output-on-failure -V
ctest --test-dir build/cy2026-windows-x86_64-py313-core --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core-runtime-free--ci-core --output-on-failure
ost build --profile lookdev --intent hydra --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra --output-on-failure
ost test --filter 'install-tree'
ost validate --strict-renderer-evidence
ost test --profile lookdev --intent hydra --filter 'install-tree'
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

The targeted core checks passed 5/5 and the full physical-GPU core suite
passed 23/23. Runtime-free checks had 13 passes and one explained GPU SKIP
out of 14. The Vulkan-only missing-driver test passed by requiring an
explained `VK_ERROR_INCOMPATIBLE_DRIVER` SKIP from the same temporal
executable in an isolated child process. Core and Hydra builds compiled the
new sources/shader without compiler warnings. After the final lifecycle
assertions were added, the temporal GPU test target was rebuilt in both
core and Hydra trees and the targeted core checks passed again.

The complete physical-GPU Hydra suite passed 46/46, including the temporal
sequences, existing material/light/integrator GPU checks and the usdview
first-frame/stable-update host smoke. Core and Hydra install-tree checks
were also run through `ost test` so their primary-report updates have a
managed producer; strict renderer-evidence validation passed in both
configurations, with explained Hydra SKIPs in core. Documentation link,
anchor, index and ownership checks passed, and the Python documentation and
release regressions passed 27/27.

Logs are `build/temporal02-core-build.log`,
`build/temporal02-targeted-test.log`, `build/temporal02-core-test.log`,
`build/temporal02-runtime-free-build.log`,
`build/temporal02-runtime-free-test.log`, `build/temporal02-hydra-build.log`
and `build/temporal02-hydra-test.log`. Final evidence validation is in
`build/temporal02-core-validate.log` and
`build/temporal02-hydra-validate.log`; the managed install-tree runs are in
`build/temporal02-core-install-test.log` and
`build/temporal02-hydra-install-test.log`.

## Scope disposition

This slice completes the backend primary-surface products and GPU history.
The buffers are available for subsequent history fetch/rejection; no GPU
shared validity or temporal radiance accumulation is claimed. Ordinary
progressive rendering remains selectable. Same-count instancer reordering
requires a caller reset; deformation conservatively loses correspondence.
Hydra temporal settings/AOV bindings and hosted moving-scene validation
remain assigned to the later integration slice. No ReSTIR, denoiser or
temporal image-quality claim is introduced.
