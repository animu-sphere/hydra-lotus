# GPU scene memory pools

Measured on 2026-10-10, based on `23f12b9` plus the memory-pool change, on
Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release and NVIDIA RTX
A5000 (Vulkan 1.4.329). Hydra used OpenUSD 26.08 lookdev and Python 3.13.
The current storage contract is in [SCENE.md](../reference/SCENE.md#gpu-scene-memory).

## Workload and observations

`renderer.scene.memory` runs after the existing upload and large-grid
timestamp checks on the same renderer, so the memory figures include their
retained staging, readback, instance and acceleration scratch capacity.
It inserts 512 independent one-triangle geometries with distinct vertex
values and 32 independent 4x4 RGBA8 textures. There is no image resolution,
sample count or stochastic seed for this buffer-content workload. Every
insertion, deletion and reinsertion reads back the entire scene and compares
geometry, instances, materials and texture texels with its CPU snapshot.

| Metric after insertion | Value |
| --- | ---: |
| Live resource ranges | 1,063 |
| Vulkan memory blocks | 3 |
| Reserved backing bytes | 37,748,736 |
| Occupied requirement bytes, including atom padding | 5,765,936 |
| Free bytes | 31,982,800 |
| Largest individual free range | 16,744,960 |

Each of three churn cycles deletes alternating geometries and textures,
then reinserts them with edited texture values. Occupied bytes fall on
deletion and return to the insertion value; reserved bytes stay fixed.
The cycles reuse 1,584 ranges with **zero additional device-memory
allocations**. Removing the scene leaves 4,307,760 occupied bytes in the
grow-only working buffers. Repeating an unchanged commit/readback leaves
all memory statistics identical, as do 1,000 unchanged bootstrap frames.
These are allocation/ownership measurements, not a frame-time baseline.

## Independent and lifecycle checks

- `lotus-renderer-memory-ranges`: 10,000 deterministic allocation/release
  steps (seed 701) checked against an independent per-byte occupancy oracle.
  The oracle checks first-fit addresses, holes, alignment, total free bytes,
  largest free spans and allocation failure; full coalescing and arithmetic
  overflow are checked separately.
- `lotus-renderer-memory-pool`: distinct host mappings in a shared block,
  released-range reuse, an explicitly requested dedicated allocation,
  oversized blocks, excess empty-block retirement, an incompatible request
  that leaves statistics unchanged and balanced allocation/free counts after
  teardown. No Vulkan validation messages.
- `lotus-renderer-memory-pool-no-driver`: an isolated missing-manifest child
  returns explained SKIP (77), preserving the physical-GPU test above.
- Existing GPU upload, texture, acceleration, timestamp, material and Hydra
  tests passed with suballocated resources. Synchronization validation
  remained enabled with zero messages. The Cornell checks at 1, 16, 64, 256
  and 1024 spp passed; rendering the committed reference's samples still
  reproduced it bit for bit. References were not regenerated.

## Commands and results

- `ost build --jobs auto`, `ost test`: **13/13 passed**.
- `ost validate --strict-renderer-evidence`: passed.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **30/30 passed**, including
  usdview first-frame/stable-update evidence.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passed.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**;
  `renderer.scene.memory` was an explained Vulkan-disabled SKIP.

The local tests exercise coherent host memory. Noncoherent padding and
flush/invalidate handling follow the
[Vulkan mapped-memory contract](https://docs.vulkan.org/spec/latest/chapters/memory.html),
but a noncoherent-only device is not measured. The lifecycle test explicitly
requests a [dedicated allocation](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryDedicatedRequirements.html);
a driver that mandates one was not available. Limits of the pooling policy
are owned by the [scene reference](../reference/SCENE.md#gpu-scene-memory).
