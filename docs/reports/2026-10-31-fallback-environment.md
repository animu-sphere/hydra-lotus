# Fallback environment image

Date: 2026-10-31

The Hydra plugin now lights a scene with no lighting of its own with OpenUSD's
Stinson Beach image, the file Storm's hosts use as their default dome texture,
instead of a white environment. usdview and `usdrecord` both get it.

## Behaviour

| Scene | Environment |
| --- | --- |
| No dome and no scene light, through the plugin | Stinson Beach, scale 1, unturned |
| The same, unreadable image | White, with a warning |
| The same, delegate created directly (tests, `lotus-viewport`) | White, as before |
| Any dome, usdview's Dome Light included | The dome's, as before |
| Any scene analytic light, even hidden, and no dome | None, as before |

usdview's Camera Light, which is on by default, is application state and
reaches the delegate as a `distantLight` sprim. It used to count as a scene
light, so usdview never showed any fallback environment. It now adds to the
fallback. `HdxTaskController` lights are told apart by the `GlfSimpleLight`
they carry under `params`, which UsdLux lights have no value for.

The image is decoded only while it lights the scene, so a stage with a dome
holds no copy of it during rendering.

## Background

A camera sample that misses shows the image along the pixel's centre, as
Storm's skydome draws it, following usdview's Dome Light Textures Visible
through `domeLightCameraVisibility`, on unless set off. It applies to any
environment image, so to textured domes as well as the fallback, and
through directly created delegates too. The radiance pass composes it from
the misses it already counted, so the accumulation is untouched: partial
coverage blends with it, the backend's default leaves headless images alone,
and toggling it needs no restart. Uniform environments stay invisible to the
camera, and a scene with no geometry records no radiance pass, so it keeps
the clear colour.

## Packaging

`adapters/hydra2/resources/StinsonBeach.hdr` and
`third_party/openusd/{LICENSE,NOTICE}.txt` come from OpenUSD's `v26.05` tag,
byte for byte; [THIRD_PARTY_NOTICES](../../THIRD_PARTY_NOTICES.md) records the
source. They are staged and installed in `lib/usd/hdLotus/assets`, and
`scripts/release.py` requires all three.

## Evidence

The host evidence log gained `environment_sampled`.

| Check | Result |
| --- | --- |
| `lotus-renderer-usdview-host`: installed plugin, real `HdxTaskController`, stage with no lights | First frame `analytic_lights=1`, `environment_sampled=1`, `ray_query=1`; asserted by the smoke test |
| `lotus-renderer-hydra-dome-texture`: the packaged image's texels | Equal to the validation image's headless decoding |
| The same test: dome, hidden sphere light and host light, added and removed | The environment follows the table; a stand-in host delegate supplies the host light |
| The same test: unreadable image | White, and no texture left in the scene |
| `lotus-renderer-hydra-dome-texture-gpu`: a hidden panel | Exactly the image's -Z region, alpha 1, through both integrators; the clear colour with the setting off, without a restart, and under a uniform environment |
| `usdrecord --renderer Lotus`: RenderMan Walking Teapot (Y-up), AutodeskTelescope and Kitchen_set (Z-up), none with lights | 64 samples each with the image sampled; reviewed by eye, not committed |

## Limitation

A delegate cannot see the stage's up axis, so the image's pole stays on world
+Y and a Z-up stage's horizon stands on end. usdview's own Dome Light turns the
image for Z-up stages and replaces the fallback.
