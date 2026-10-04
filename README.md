# Hydra Lotus

> A GPU light transport research renderer for OpenUSD / Hydra: physically
> based path tracing, built on a reference path tracer that every faster mode
> is checked against.

## Scope

`hydra-lotus` is a Hydra render delegate for **physically based Monte Carlo
rendering**: Vulkan first, wavefront path tracing, NEE / MIS, ReSTIR,
temporal reconstruction and denoising, with room for advanced sampling and
spectral research on top. It is built in a fixed order — correctness, then
measurement, then optimization, then research — and keeps a reference path
tracer to check every faster mode against
([roadmap policy](docs/design/ROADMAP_POLICY.md)).

It **owns** the renderer: the Hydra-independent core, the Vulkan backend, the
`hdLotus` Hydra adapter, the integrators, the sampling, reuse and denoising
passes, and the material translation into its own material representation.

It **does not own** the USD schemas or the material standards it reads —
those are OpenUSD's, MaterialX's and OpenPBR's — or the build and runtime
tooling, which is [OpenStrata](https://github.com/animu-sphere/open-strata)'s.
Raster rendering is [`hydra-merlin`](https://github.com/animu-sphere/hydra-merlin)'s
role and avatar / toon rendering is
[`hydra-toon`](https://github.com/animu-sphere/hydra-toon)'s; neither is a
dependency ([integration scope](docs/design/INTEGRATION_SCOPE_POLICY.md)).

## Architecture

```text
OpenUSD / Hydra ─→ hdLotus adapter ─→ LotusScene ─→ GpuScene ─→ Integrator ─→ AOV / denoise / present
                    (scene extraction,   (CPU,        (BLAS/TLAS,  (reference PT,
                     dirty tracking,      renderer-    bindless     wavefront PT,
                     material translate)  neutral)     tables)      ReSTIR)
```

Hydra is a scene integration layer. The light transport is independent of it,
so a headless runner, a benchmark or a test-scene runner drive the same core.
See the [design policy](docs/design/DESIGN_POLICY.md).

## Components

| Component | Directory | Role |
| --- | --- | --- |
| `lotus-render-world` | `core/render-world/` | host-neutral scene state |
| `lotus-render-extraction` | `core/render-extraction/` | scene changes → GPU update work |
| `lotus-render-vulkan` | `backend/vulkan/` | Vulkan backend and Slang shaders |
| `hdLotus` | `adapters/hydra2/` | Hydra render delegate and renderer plugin |
| `lotus-headless` | `adapters/headless/` | headless runner and renderer evidence |
| `lotus-viewport` | `adapters/viewport/` | standalone window |

What each can do today is the
[capability matrix](docs/reference/CAPABILITY_MATRIX.md); where new code goes
is [PROJECT_LAYOUT.md](docs/architecture/PROJECT_LAYOUT.md).

## Documentation

[docs/](docs/README.md) — [design](docs/design/DESIGN_POLICY.md),
[roadmap policy](docs/design/ROADMAP_POLICY.md),
[layout](docs/architecture/PROJECT_LAYOUT.md),
[capabilities](docs/reference/CAPABILITY_MATRIX.md),
[roadmap](docs/roadmap/README.md), [building](docs/guides/BUILDING.md),
[changelog](CHANGELOG.md).

## Build

The project is built with [OpenStrata](https://github.com/animu-sphere/open-strata)
(`ost`) and generated from its `renderer` template:

```sh
ost build --jobs auto
ost test
ost validate
```

The default build needs Vulkan 1.3 and `slangc` and no OpenUSD. The Hydra
adapter, the viewport and plain CMake are in [BUILDING.md](docs/guides/BUILDING.md).

## License

[Apache-2.0](LICENSE).
