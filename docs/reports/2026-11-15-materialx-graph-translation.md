# MaterialX graph translation

## Evaluation brief

LOTUS-MATERIALX-04 implements the accepted
[translation contract](../design/MATERIALX_TRANSLATION.md): the Hydra
network as the one input path, graphs folded exactly into a constant or one
lookup, the supported node table, colour spaces and structured diagnostics.
Adopt it if the translator folds the table's nodes and no others, every
unsupported surface, terminal, node, graph, value and input raises a
diagnostic, the definitions it holds are the pinned runtime's,
`material-graphs-v1` is committed with references under both integrators
and agrees between Hydra and headless, and every existing committed
reference is unchanged
([acceptance](../design/MATERIALX_TRANSLATION.md#9-acceptance-for-lotus-materialx-04)).
Five points depart from or sharpen the contract's text; review accepted
them ([below](#changes-to-the-contract)).

## What changed

- **Graph reader**
  ([`material_graph.cpp`](../../adapters/hydra2/src/material_graph.cpp)).
  A value is a constant, one lookup whose texel channels each carry a
  scale, a bias and a fallback in double precision, or a coordinate set
  under an affine map. Each node family has one rule; a rule either
  returns such a value or records where it stopped. 212 node definitions
  are held as a table of inputs, types and defaults, generated per type,
  which the rules read their defaults from. No MaterialX library is linked.
- **Standard Surface reader**
  ([`material_translator.cpp`](../../adapters/hydra2/src/material_translator.cpp)).
  Reads every input of the 42 in its definition as translated,
  unrepresented or without effect, and reports parameters and connections
  the definition does not name. The product of a weight and its colour is
  the graph reader's product.
- **Diagnostics.** `HdLotusMaterialTranslation::diagnostics` replaces
  `unsupported`, `connected_inputs` and `unrepresented_inputs`: a kind, the
  input, the node's path and identifier, and the rule. The `UsdPreviewSurface`
  reader reports through the same list. The adapter warns a line per
  outcome and a line per input, and `asset_survey.py` classifies both.
- **Fixture.** `material-graphs-v1`, 25 cards of Standard Surface behind
  node graphs, with its headless mirror, its references and a negative
  control. `graph-stage-v1.usda` and the `graph-stage-v1.mtlx` it
  references hold one material twice.
- **Tests.** `lotus-renderer-hydra-material-graph`, in the `fast` tier.

The three behaviours the contract called silent are gone: an image in
another colour space raises `Substituted`, an authored surface input the
reader does not name `UnknownInput`, and a displacement or volume terminal
`UnsupportedTerminal`.

## Results

Windows 11, MSVC 14.51, OpenUSD 26.08 with MaterialX 1.39.5 from the
`lookdev` runtime, NVIDIA RTX A5000, Release.

| Check | Result |
| --- | --- |
| `lotus-renderer-hydra-material-graph` | Passes: node table and exclusions, compound equivalence, definitions, completeness, stage round trip |
| Definitions against `SdrRegistry` | 212 node definitions and 577 defaults are the runtime's; the surfaces are glTF PBR 2.0.1, Standard Surface 1.0.1 and OpenPBR 1.1.1 |
| `lotus-renderer-hydra-material-fixtures` | All 11 fixtures extract to their mirrors exactly; `material-graphs-v1`'s four pairs agree |
| `lotus-renderer-hydra-material-fixtures-gpu` | Every fixture is its mirror's image bit for bit under NEE/MIS and BSDF sampling and matches its reference; `material-graphs-v1` image z −0.19 −0.15 −0.14 (NEE/MIS) and 0.17 −0.53 −0.74 (BSDF) at 256 spp |
| `renderer.material.fixtures` (headless) | Passes; `material-graphs-v1` under the reference and wavefront integrators, its control (glTF rotation reversed) rejected at a largest tile z of 108.90 |
| Existing references | `--write-material-reference` rewrote 20 existing files byte-identically and added two |
| Hydra `fast` tier | 42 of 42 pass; `-material-gpu` and `-texture-gpu` pass |
| Script tests, `check_docs.py`, `material_fixtures.py check` | Pass |

Four deliberate defects each fail a test: glTF's rotation not negated
(compound equivalence, and the fixture's mirror), an sRGB constant left
undecoded (node table, and the mirror), `ND_multiply`'s second default set
to 0 (definitions), and a constant minus a lookup read as the lookup minus
the constant (node table, and the mirror).

No existing fixture material gains a diagnostic. The materials that warn
are the ones that warned before: `material-mtlx-v1`'s Lobes and
Untranslated, `material-transmission-v1`'s Depth, NoSpecular, Ignored and
Graph, and the `UsdPreviewSurface` fixtures' untranslated connections. Each
now also names its node and rule.

## glTF sample assets

The translator has no glTF PBR surface until LOTUS-MATERIALX-05, so a glTF
material still stops at `UnsupportedSurface` before any graph is read.
What its graphs will meet was measured ahead: the node identifiers
[usd-gltf-plugins](https://github.com/animu-sphere/usd-gltf-plugins)
authors for the Khronos glTF sample assets, opened as stages through its
file format plugin under the `lookdev` runtime, against this task's table.
142 assets opened (8 were rejected by the plugin) with 11,776 materials.

| Node | Shaders | Assets | In the table |
| --- | --- | --- | --- |
| `ND_gltf_pbr_surfaceshader` | 11,776 | 142 | Surface of LOTUS-MATERIALX-05 |
| `ND_geompropvalue_vector2` | 10,656 | 118 | Yes |
| `ND_image_color4`, `ND_convert_color4_color3` | 10,524 each | 109 | Yes |
| `ND_extract_vector4` | 783 | 79 | Yes |
| `ND_image_vector4` | 748 | 82 | Yes |
| `ND_convert_vector4_vector3`, `ND_normalmap_float` | 167 each | 53 | Yes |
| `ND_multiply_vector2`, `ND_rotate2d_vector2`, `ND_add_vector2` | 148 each | 15 | Yes |
| `ND_multiply_color3` | 84 | 20 | Yes |
| `ND_multiply_float` | 73 | 30 | Yes |
| `ND_extract_color4` | 51 | 21 | Yes |
| `ND_mix_float` | 36 | 7 | Yes |
| `ND_multiply_vector4`, `ND_subtract_vector4`, `ND_convert_vector4_vector2` | 19 each | 6 | Yes |
| `ND_dotproduct_vector2`, `ND_atan2_float`, `ND_ifgreater_float` | 19 each | 6 | No |
| `ND_geompropvalue_color3` | 13 | 7 | No |
| `ND_geompropvalue_float` | 2 | 2 | No |
| `ND_add_float` | 1 | 1 | Yes |

Every image carries `srgb_texture` or no colour space. The plugin authors
no `ND_gltf_image`, `ND_gltf_colorimage` or `ND_gltf_normalmap`: it writes
their content in primitive nodes, all of which the table holds. The five
identifiers outside it are the anisotropy direction (`dotproduct`, `atan2`
and `ifgreater`, on six assets), which is not affine in a lookup, and
vertex colour and alpha (`geompropvalue_color3` and `_float`), which
LOTUS-MATERIALX-05 has to decide on with glTF's base colour.

This is an inventory of authored identifiers, not of translations: whether
each graph folds — one coordinate set per material, one lookup per input —
is measured when the surface exists.

## Changes to the contract

These are listed in the
[contract](../design/MATERIALX_TRANSLATION.md#11-changes-found-in-implementation)
and were accepted in review, 2026-11-15.

1. **`material-mtlx-v1`'s USD file changed by one node.** Its Untranslated
   card multiplied an image by 0.5 to show an unread node. That product
   now folds, which would have changed the card and the reference; the
   node is now `ND_power_color3FA`, which does not fold. The reference
   images are byte-identical.
2. **Sdr does not carry a MaterialX definition's version.**
   `SdrShaderNode::GetShaderVersion` is invalid for these nodes, so the
   definitions test reads each surface's version from the document Sdr
   resolved the node to.
3. **`ND_gltf_colorimage` follows its library graph**, which fixes
   operation order 0 and periodic addressing whatever the node's own
   inputs say. And `ND_gltf_normalmap` and `ND_normalmap` of one image are
   one lookup except for the value without the image (the flat normal
   against `ND_image`'s black), so the fixture's pair is compared apart
   from the fallback; with the image present the cards are the same.
4. **OpenUSD's colour space names.** `lin_rec709_scene` and
   `srgb_rec709_scene` read as `lin_rec709` and `srgb_texture`: UsdImaging
   hands over what the stage authored, and these are OpenUSD's names for
   the same two spaces.
5. **Smaller points.** The warning's summary line groups the three
   `Unsupported` graph kinds, which have one outcome. A clamp on
   `base_color` or `emission_color` is the input's own only under a weight
   of 1. A coloured `opacity` is `Substituted`, since its mean is used. A
   connected `mix` weight that folds to a constant is a constant. An
   authored input a node's definition does not name is `UnsupportedValue`.

## Limits

- MSVC 14.51 failed with an internal compiler error (C1001) on a test
  helper that returned a pair of vectors from a lambda into a structured
  binding; the helper is a function returning a struct.
- The stage round trip compares the two materials' IR, which the delegate
  exposes, not their diagnostics.
- The glTF inventory used a workstation build of the plugin and local
  sample assets; neither is part of this repository or its CI.
