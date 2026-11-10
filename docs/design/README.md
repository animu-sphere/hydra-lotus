# Design

Stable decisions and rationale. Implementation behavior and limitations belong
in [reference](../reference/README.md); phase and release status belongs in the
[canonical roadmap](../roadmap/README.md#status-at-a-glance).

| Document | Owns |
| --- | --- |
| [POST_V060_STRATEGY.md](POST_V060_STRATEGY.md) | Practical-renderer priorities, measurement/adoption rules and research stop rules. |
| [DESIGN_POLICY.md](DESIGN_POLICY.md) | Renderer purpose, boundaries, light transport design and decision principles. |
| [ROADMAP_POLICY.md](ROADMAP_POLICY.md) | Renderer phase goals, scope, exit criteria and validation strategy. |
| [INTEGRATION_SCOPE_POLICY.md](INTEGRATION_SCOPE_POLICY.md) | Repository ownership and consumption of external contracts. |
| [TEST_POLICY.md](TEST_POLICY.md) | Test tiers, where each runs, budgets, and admitting and retiring tests. |
| [TRANSMISSION.md](TRANSMISSION.md) | The dielectric transmission lobe's transport contract: IR fields, interface sides, BTDF, sampling, light sampling and translation. |
| [INTERACTIVE_QUALITY.md](INTERACTIVE_QUALITY.md) | The presets and invalidation contract: Interactive, Balanced and Reference, settling, change categories, and measuring still and moving behaviour separately. |
| [MATERIAL_IR_V2.md](MATERIAL_IR_V2.md) | Material IR v2: tagged shading models on the CPU, the fixed-stride GPU record with a binding table for lookups, switch dispatch, and the pinned MaterialX version. |
| [BSDF_API.md](BSDF_API.md) | The BSDF API every shading model implements: `Evaluate`, `Pdf` and `Sample`, their frame and measures, delta and transmission components, what the integrators guarantee, reservoir packing, and the CPU consistency tests. |
| [MATERIALX_SHADING_MODELS.md](MATERIALX_SHADING_MODELS.md) | The MaterialX shading-model direction: glTF PBR, Standard Surface and OpenPBR as tagged models in material IR v2, one BSDF API, layering, graph-support boundary, tests and acceptance. |
