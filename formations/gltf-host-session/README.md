# glTF host session

This OpenStrata Formation opens a glTF file directly in `testusdview` and
renders it with Lotus. It composes three digest-pinned artifacts: the
`lookdev` OpenUSD runtime, the `usdGltfFileFormat` plugin of
[usd-gltf-plugins](https://github.com/animu-sphere/usd-gltf-plugins), which
authors each glTF material as a MaterialX glTF PBR surface, and the Lotus
Hydra package, which renders that surface as its
[glTF PBR model](../../docs/reference/SCENE.md#gltf-pbr).

The smoke test checks that the stage came through the glTF file format, that
its three materials are `ND_gltf_pbr_surfaceshader` surfaces and that Lotus is
the active renderer, then captures the three cards — a red dielectric, a
green metal and a blue emitter — to `gltf-cards.png` and counts their pixels.
`sample.gltf` is synthetic and ships with this repository.

The session targets Windows x86_64 with Python 3.13, OpenUSD 26.08 `lookdev`
and a Vulkan 1.3 device with ray queries. It was last run on 2026-11-16 with
`ost` 0.23.20, usd-gltf-plugins v0.9.0 and Lotus at 0.9.0.

## Prepare the artifacts

Pull the runtime pinned in the manifest:

```powershell
ost artifact pull oci://ghcr.io/animu-sphere/openstrata-runtime-cy2026-lookdev@sha256:b840ed4690aa39d4582bc03c0a09fa7bab717216635b55a4eb718482dbfb196b --expect-artifact sha256:b982656c07dd9147973e3c7197d9b050ee48dd1ac291988f7e6025c76c4a785b --require-kind runtime
ost runtime pull cy2026 --profile lookdev --from-artifact sha256:b982656c07dd9147973e3c7197d9b050ee48dd1ac291988f7e6025c76c4a785b
```

The plugin and the renderer packages must both target that exact runtime. The
published usd-gltf-plugins product targets the `usd` profile, so build a
`lookdev` package from its source. In a `usd-gltf-plugins` checkout:

```powershell
ost plugin build plugins/usdGltfFileFormat --profile lookdev
ost plugin package plugins/usdGltfFileFormat --profile lookdev
ost artifact import plugins/usdGltfFileFormat/dist/plugins/usdGltfFileFormat/<version>/cy2026-windows-x86_64-py313-lookdev
```

In this checkout, with the prerequisites of the
[build guide](../../docs/guides/BUILDING.md):

```powershell
ost build --profile lookdev --intent hydra --jobs auto
ost package --profile lookdev --intent hydra
ost artifact import dist/lotus/<version>/cy2026-windows-x86_64-py313-lookdev--hydra
```

Each import prints the artifact's full `sha256:...` digest.

## Lock and run

Run from `formations/gltf-host-session`, where the test and fixture paths
resolve. Set `$usdGltfDigest` and `$lotusDigest` to the two digests, then in
PowerShell 7:

```powershell
(Get-Content -Raw formation.toml.in).Replace('@USD_GLTF_ARTIFACT@', $usdGltfDigest).Replace('@LOTUS_ARTIFACT@', $lotusDigest) |
    Set-Content -Encoding utf8NoBOM formation.toml
ost formation lock formation.toml
ost formation doctor formation.toml
ost formation run formation.toml
```

A passing run ends with `Lotus drew the glTF fixture` and the three pixel
counts. `testusdview` also prints OpenGL teardown errors from the host's own
presentation after the script has passed; the run's exit code is the
script's.

To view an asset interactively, pass a command after `--`:

```powershell
ost formation run formation.toml -- usdview sample.gltf --renderer Lotus
ost formation run formation.toml -- usdview C:\path\to\Model.glb --renderer Lotus
```

`formation.toml`, `formation.lock`, the captured image and the run evidence
under `.strata/` record one workstation's artifacts and are not committed.
Publishing the session needs both packages published for the `lookdev`
runtime, and the resulting `formation.toml` and `formation.lock` checked in
with their pull instructions.
