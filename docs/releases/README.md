# Release records

Each released version gets an immutable record here: its objective, the
capabilities it shipped, compatibility notes, and known limitations. Release
records are history — once written for a released version they are not
rewritten; new work goes to a new record. Active, incomplete work lives in the
[roadmap](../roadmap/), not here.

The [release procedure](../guides/RELEASING.md) owns tag-driven Actions,
dry runs, asset naming and publication.

| Version | Record | Theme |
| --- | --- | --- |
| [v0.5.2](v0.5.2.md) | [Viewport foundation release](v0.5.2.md) | Standalone viewport presenting progressive path tracing without readback, USD stages through Hydra, usdview navigation, run-time renderer controls, GPU debug views and a timing HUD. |
| [v0.5.1](v0.5.1.md) | [ReSTIR DI closure release](v0.5.1.md) | Exit review of the ReSTIR DI investigation: NEE/MIS stays the default, initial-only reservoirs for many lights of similar importance, reuse not worth using as a renderer path. |
| [v0.5.0](v0.5.0.md) | [Experimental ReSTIR DI release](v0.5.0.md) | Opt-in initial, temporal, spatial and combined direct-light reservoirs, Hydra controls/AOVs and measured quality/cost limits. |
| [v0.4.0](v0.4.0.md) | [Temporal infrastructure release](v0.4.0.md) | Frame history, rigid motion, reprojection, shared validity and opt-in bounded radiance under both integrators, with Hydra controls and debug AOVs. |
| [v0.3.0](v0.3.0.md) | [Direct lighting release](v0.3.0.md) | Next event estimation with MIS towards emissive geometry, UsdLux analytic lights and textured domes, under both integrators and through Hydra. |
| [v0.2.0](v0.2.0.md) | [Wavefront release](v0.2.0.md) | Wavefront path tracing that reproduces the reference transport, selectable headless and through Hydra, and the first tag-driven Windows Hydra package. |
| [v0.1.0](v0.1.0.md) | [Foundation release](v0.1.0.md) | Deterministic Vulkan reference rendering for OpenUSD Hydra, minimal materials and measured backend behavior. |
