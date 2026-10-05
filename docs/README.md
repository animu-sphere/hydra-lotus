# hydra-lotus documentation

Documentation is organized by responsibility, and each category answers one
class of question — the layout `hydra-merlin`, `hydra-toon`,
`usd-vrm-plugins` and `open-strata` share. Every subject has one owning
document; this page says which.

When a document disagrees with the implementation, the implementation wins
and the document is a bug. When a summary disagrees with
[architecture/PROJECT_LAYOUT.md](architecture/PROJECT_LAYOUT.md) about
*structure*, the layout wins.

| Category | Answers | Start here |
| --- | --- | --- |
| [design/](design/) | What the renderer is meant to be, why, and in what order it is built. | [DESIGN_POLICY.md](design/DESIGN_POLICY.md) · [ROADMAP_POLICY.md](design/ROADMAP_POLICY.md) |
| [architecture/](architecture/) | Which targets exist, where code goes, and how they depend on each other. | [PROJECT_LAYOUT.md](architecture/PROJECT_LAYOUT.md) |
| [reference/](reference/) | What is implemented now, and on what. | [CAPABILITY_MATRIX.md](reference/CAPABILITY_MATRIX.md) · [SUPPORTED_CONFIGURATIONS.md](reference/SUPPORTED_CONFIGURATIONS.md) · [AOVS.md](reference/AOVS.md) |
| [roadmap/](roadmap/) | What incomplete work remains. | [README.md](roadmap/README.md) · [current.md](roadmap/current.md) |
| [guides/](guides/) | How to perform a task. | [BUILDING.md](guides/BUILDING.md) |
| [releases/](releases/) | What shipped in a released version. | [README.md](releases/README.md) |
| [reports/](reports/) | What was measured or observed. | [README.md](reports/README.md) |
| [archive/](archive/) | What used to be planned or authoritative and is now superseded. | [README.md](archive/README.md) |
| [contributing/](contributing/) | How these documents are maintained. | [documentation.md](contributing/documentation.md) |

## Source of truth

| Question | Owner |
| --- | --- |
| Purpose, principles, renderer architecture, light transport design | [design/DESIGN_POLICY.md](design/DESIGN_POLICY.md) |
| Renderer Phase 0–10: goals, scope, exit criteria; milestones; testing strategy; near-term priority | [design/ROADMAP_POLICY.md](design/ROADMAP_POLICY.md) |
| What this repository owns, what it consumes from whom, what it does not own | [design/INTEGRATION_SCOPE_POLICY.md](design/INTEGRATION_SCOPE_POLICY.md) |
| Targets, names, directories, dependency directions, build intents, install tree | [architecture/PROJECT_LAYOUT.md](architecture/PROJECT_LAYOUT.md) |
| Implemented capabilities; measured platforms and runtimes | [reference/](reference/) |
| Foundation AOV set, formats, storage and clear behaviour | [reference/AOVS.md](reference/AOVS.md) |
| CPU mesh geometry, placement, snapshots, Hydra extraction, the scene update plan and the GPU scene | [reference/SCENE.md](reference/SCENE.md) |
| Incomplete work, and which release carries it | [roadmap/](roadmap/) |
| Released history | [releases/](releases/) and the [CHANGELOG](../CHANGELOG.md) |
| How documents here are maintained, and how they cite other repositories | [contributing/documentation.md](contributing/documentation.md) |

Where the design documents overlap, the narrower one wins:
INTEGRATION_SCOPE_POLICY.md and ROADMAP_POLICY.md over DESIGN_POLICY.md on
their subjects, and none of them over PROJECT_LAYOUT.md on structure.

## Owned elsewhere

This repository consumes these subjects and does not define them. Each is
linked, never restated:

| Subject | Owner |
| --- | --- |
| USD schemas (`UsdGeom`, `UsdLux`, `UsdShade`, `UsdRender`), Hydra and the `UsdPreviewSurface` specification | [OpenUSD](https://openusd.org/release/index.html) |
| MaterialX and OpenPBR | [MaterialX](https://materialx.org/) · [OpenPBR](https://academysoftwarefoundation.github.io/OpenPBR/) |
| Build, runtimes, renderer evidence and validation | [`open-strata`](https://github.com/animu-sphere/open-strata/tree/main/docs) |
| Raster rendering of USD scenes | [`hydra-merlin`](https://github.com/animu-sphere/hydra-merlin/tree/main/docs) |
| Avatar and toon rendering | [`hydra-toon`](https://github.com/animu-sphere/hydra-toon/tree/main/docs) |
