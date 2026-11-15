---
status: accepted
owner: hydra-lotus
---

# MaterialX graph translation

> Status: **accepted**, 2026-11-15, without review changes. This is LOTUS-MATERIALX-04's contract,
> written before any code: how a MaterialX surface and the node graph that
> feeds it reach the [material IR](MATERIAL_IR_V2.md), which nodes
> translate, and what happens to everything else. It settles the
> [shading-model contract](MATERIALX_SHADING_MODELS.md#10-decisions-for-the-design-review)'s
> input paths (§10.4). It adds no shading model and changes no committed
> reference: the models' own inputs arrive with LOTUS-MATERIALX-05, 07 and
> 08. It is implemented
> ([report](../reports/2026-11-15-materialx-graph-translation.md)), with
> [changes found in implementation](#11-changes-found-in-implementation)
> accepted in review. The [scene reference](../reference/SCENE.md#materials) owns the
> implemented behaviour. "Today" below describes `main` after
> LOTUS-MATERIALX-03.

## 1. Scope

The translator's input, the form a graph is folded into, the supported
node table with each node's rule, the diagnostics every other node,
connection and value raises, and the tests that fix colour space,
channels, texture coordinates, normal space and unconnected defaults.

Not in this document: any model's input table, slots, ranges and lobes
(05–08), the expression program [IR v2 §4.2](MATERIAL_IR_V2.md#42-inputs-and-texture-expressions)
rejected, a second texture-coordinate source per material, and UDIM.

## 2. Today

- [`material_translator.cpp`](../../adapters/hydra2/src/material_translator.cpp)
  reads Hydra's material network under the `mtlx` render context and links
  no MaterialX library. A `UsdPreviewSurface` and an
  `ND_standard_surface_surfaceshader` translate into `PreviewSurface`;
  any other surface warns and leaves the default material.
- Its `GraphReader` folds `ND_image_*`, untiled `ND_tiledimage_*`,
  `ND_texcoord_vector2`, `ND_geompropvalue_vector2`, `ND_extract_*`,
  `ND_convert_*` and `ND_normalmap*` into a constant or one lookup. The
  only arithmetic is the product Standard Surface's weights need, written
  in the surface reader, not as a node.
- Diagnostics are three fields: `unsupported` (a sentence),
  `connected_inputs` and `unrepresented_inputs` (input names). They say
  which input fell back, not which node or rule stopped it.
- Three behaviours are silent, which the
  [contract](MATERIALX_SHADING_MODELS.md#5-boundary-of-materialx-graph-support)
  forbids: an image in a colour space other than sRGB's is read as raw; an
  authored surface input the reader does not name is not looked at; and a
  `displacement` or `volume` terminal is ignored.

## 3. Input paths (contract §10.4)

**One path: the Hydra material network.** A MaterialX surface reaches
Lotus as the network `GetMaterialResource` returns under the `mtlx`
context, whichever way it was authored:

| Authored as | Arrives as |
| --- | --- |
| `UsdShade` shaders with `ND_*` identifiers on the stage | that network |
| A `.mtlx` document referenced into the stage | the same, through OpenUSD's `usdMtlx` file format |
| A node graph prim, or an interface input | its nodes and resolved values; UsdImaging flattens both |

Lotus opens no `.mtlx` file itself, as the
[integration scope](INTEGRATION_SCOPE_POLICY.md#1-the-rule) already
requires for glTF. The headless adapter keeps building the IR directly
from [fixtures mirrored in code](../reference/SCENE.md#material-fixtures);
it gains no MaterialX reader.

**The translator keeps linking no MaterialX library.** A library node
implemented as a node graph, `ND_gltf_colorimage` for example, arrives
unexpanded, so the translator carries its own rule for each node it
supports (§5) and diagnoses the rest (§6). What that costs is a rule per
compound node, about ten for the subset below; what it keeps:

- The adapter's link set stays Hydra and UsdImaging. It builds only under
  the `lookdev` profile, which does carry MaterialX, so this is a
  preference for a small surface, not a constraint.
- The supported set is a table in this repository, not whatever a host's
  library version expands, so the same stage translates the same way on
  every host.
- Hand-written rules can drift from the library. §7's definition test
  holds them to the pinned node definitions through `Sdr`, which OpenUSD
  already provides.

**Rejected: `hdMtlx` and `flattenSubgraphs`.** Rebuilding a MaterialX
document from the network and letting the library expand compound nodes
would remove the compound rules, but the primitive nodes still need
folding and diagnosing, the result would follow the host's library
version rather than [the pinned one](MATERIAL_IR_V2.md#3-versions-contract-103),
and the adapter would link `hdMtlx` and MaterialX for it.

**A custom node definition** in a document arrives as a node whose
identifier the table does not hold, and is diagnosed like any other
unsupported node.

## 4. What a graph folds into

[IR v2](MATERIAL_IR_V2.md#42-inputs-and-texture-expressions) fixes an
input's expression: a constant, or one lookup with a per-channel affine
map, a channel choice, a texture-coordinate transform and the input's
clamp. A graph translates when it folds into that form:

```text
value := constant[1..4]
       | lookup(image, wrap, st_transform) · scale + bias   one channel or RGB(A)
```

Folding is exact or it does not happen. A graph that would need two
lookups, a non-affine function of a lookup, or a clamp other than the
input's own does not translate, and the input is diagnosed (§6); the
translator never approximates a graph.

A material still has one texture-coordinate source, its first lookup's.
Each lookup has its own `st_transform`, so tiling and placement differ per
input; a second coordinate *set* does not translate.

## 5. Supported nodes

Identifiers are MaterialX 1.39.5's. `*` stands for the float, colour and
vector types of one to four components unless a rule narrows it. "Constant"
below includes an unconnected input's default.

### 5.1 Values and channels

| Node | Rule |
| --- | --- |
| An authored value; `ND_constant_*`; `ND_dot_*` | The value; `dot` passes its input through |
| `ND_convert_*` | As today: a scalar constant spreads; a lookup converts only between three- and four-component types |
| `ND_extract_*` | A constant's component, or a lookup's channel, at a constant `index` |
| `ND_separate2_*`, `ND_separate3_*`, `ND_separate4_*` | The same, chosen by the connection's output name (`outx`…`outw`, `outr`…`outa`) |
| `ND_combine2_*`, `ND_combine3_*`, `ND_combine4_*` | Constants; or channels 0, 1, 2 (and 3) of one lookup in order, each with its own scale and bias, which is that lookup again |

### 5.2 Arithmetic

| Node | Constant inputs | With one lookup |
| --- | --- | --- |
| `ND_add_*`, `ND_subtract_*` | Folded | The other side moves `bias` (and negates `scale` for a subtrahend lookup) |
| `ND_multiply_*`, and `ND_divide_*` by a non-zero constant | Folded | The constant scales `scale`, `bias` and `fallback`; the `FA` forms scale every channel |
| `ND_invert_*` | Folded | `amount − in`, with a constant `amount` |
| `ND_mix_*` | Folded | A constant `mix` and one constant side: `bg·(1 − mix) + fg·mix` is affine in the lookup |
| `ND_clamp_*` | Folded | Only as the last node before a surface input whose own range lies inside `[low, high]`, where the clamp does nothing the input does not |
| `ND_min_*`, `ND_max_*`, `ND_power_*`, `ND_absval_*`, `ND_sqrt_*`, `ND_floor_*`, `ND_ceil_*` | Folded | Not translated |

Two lookups under any of these, a scalar lookup spread over a colour, and
a connected `mix` weight, `index`, `low` or `high` do not translate.
Division by a constant with a zero component does not either. Constant
folding is done in double precision, then stored as the IR's floats.

### 5.3 Images and coordinates

| Node | Rule |
| --- | --- |
| `ND_image_*` | As today: `file` and its colour space (§5.5) name the texture, `default` is the fallback and the value without a file, `uaddressmode` and `vaddressmode` map to the IR's wraps, `texcoord` gives the coordinates |
| `ND_tiledimage_*` | The same with `st_transform` = `uv · uvtiling − uvoffset`, periodic in both directions. Real-world sizes are not translated |
| `ND_gltf_image_*` | An image whose coordinates pass `ND_place2d_vector2` with the node's `pivot`, `scale`, `rotate`, `offset` and `operationorder` as its graph applies them (inverted scale, negated rotation, offset negated in u), times `factor` |
| `ND_gltf_colorimage` | The colour-4 `ND_gltf_image` times `color` and a constant `geomcolor`; `outcolor` is RGB and `outa` channel 3 |
| `ND_gltf_normalmap_vector3_1_0` | The vector-3 `ND_gltf_image` under `ND_normalmap` with scale 1 |
| `ND_texcoord_vector2` | The `st` set for index 0, as today |
| `ND_geompropvalue_vector2` | The named set, with `default` as its fallback, as today |
| `ND_place2d_vector2` | An affine map composed into `st_transform`, in either `operationorder`, from constant parameters |
| `ND_multiply_vector2`, `ND_add_vector2`, `ND_subtract_vector2`, `ND_divide_vector2`, `ND_rotate2d_vector2` on coordinates | The same, with the other side constant |

`filtertype` `linear` is the IR's; `closest` and `cubic` are diagnosed and
read as linear. `framerange` and the other sequence inputs are diagnosed
when authored. `constant` addressing stays exact only where `default` is
zero, and a non-zero one is diagnosed rather than accepted in silence as
today.

### 5.4 Normals

`ND_normalmap_float` and `ND_normalmap_vector2` translate as today: a
three-component image or constant, signed by scale 2 and bias −1, x and y
times a constant `scale`, in tangent space with the default `normal`,
`tangent` and `bitangent`. The result feeds only a surface's normal
inputs; a normal map on a colour input, and a colour graph on a normal
input, are diagnosed. Which normal inputs a model has — a coat's as well
as the base's — is that model's table.

The tangent frame is the [scene's](../reference/SCENE.md#materials-and-environment),
built from the lookup's coordinate set as for `UsdPreviewSurface`.
`ND_gltf_normalmap` is expected to need no flip against it, since its
library graph is a plain `ND_normalmap`; a fixture decides (§7), and a
flip found there is recorded in the report rather than assumed here.

### 5.5 Colour space

The working space is linear Rec.709, MaterialX's `lin_rec709`.

| Authored colour space | On an image `file` | On a constant colour |
| --- | --- | --- |
| None, `lin_rec709`, `raw`, `data`, `none` | Raw | As authored |
| `srgb_texture` | Decoded by the texture's sRGB format | Decoded on the CPU when folding |
| Anything else (`acescg`, `g22_rec709`, `srgb_displayp3`, …) | Raw, **diagnosed** | As authored, **diagnosed** |

A colour space on a float or vector image is ignored without a
diagnostic, as MaterialX ignores it. UsdImaging hands a colour space over
as `colorSpace:<input>` beside the parameter; the translator reads that
entry for every colour parameter it folds, not only `file`.

## 6. Diagnostics

### 6.1 The record

The three fields of §2 become one list, so a test names what it expects
and a user learns what to change:

```cpp
enum class HdLotusMaterialDiagnosticKind {
  UnsupportedSurface,    // the terminal's shader has no model: default material
  UnsupportedTerminal,   // displacement or volume is connected: not rendered
  UnsupportedNode,       // a node identifier the table does not hold
  UnsupportedGraph,      // supported nodes in a shape §4 cannot hold
  UnsupportedValue,      // a parameter value outside a node's rule
  UnrepresentedInput,    // the model has no place for the input's value
  UnknownInput,          // an authored input the model's table does not name
  Substituted,           // translated with a stated difference
};

struct HdLotusMaterialDiagnostic {
  HdLotusMaterialDiagnosticKind kind;
  TfToken input;      // the surface input affected; empty for a terminal
  SdfPath node;       // where translation stopped
  TfToken node_type;  // its identifier
  std::string detail; // the rule, in one clause: "two lookups under ND_multiply_color3"
};
```

### 6.2 What each kind does

The contract allows three outcomes: warn, substitute explicitly, or fail
to load. Hydra offers no way to fail a material's load, so the strongest
outcome is the default material with a warning.

| Kind | Result in the image | Message |
| --- | --- | --- |
| `UnsupportedSurface` | The default material | As today |
| `UnsupportedTerminal` | The surface renders; the terminal does nothing | Once per material |
| `UnsupportedNode`, `UnsupportedGraph`, `UnsupportedValue` | The input takes its node definition's default | Input, node path, identifier and rule |
| `UnrepresentedInput`, `UnknownInput` | The input is ignored | Input and value |
| `Substituted` | The stated substitute: raw for a colour space, linear for a filter, black outside for `constant` addressing | Input, node and what was substituted |

One input raises one diagnostic: the node nearest the surface at which
folding stopped. The adapter warns whenever it translates the material, as
today, one line per kind listing its inputs, followed by one line per
diagnostic with the node path. [`asset_survey.py`](../../scripts/asset_survey.py)
classifies these lines, so it changes with them.

**A translation without diagnostics is complete**: every authored input,
connection and terminal was read and is represented exactly. That
sentence is the property the tests assert, and what "Supported" means in
the [compatibility table](MATERIALX_SHADING_MODELS.md#9-scope-tracking-and-acceptance).

**Rejected: a visible error material.** A magenta surface for a diagnosed
material would be harder to miss than a warning, but a material with one
unsupported input out of ten would lose the nine that work, and which of
the two a user wants depends on the task. It can return as a setting if
warnings prove too quiet.

### 6.3 Authored inputs the table does not name

A surface reader lists every input of its model's node definition, each as
translated, unrepresented, or without effect in a path tracer (glTF's
`occlusion`). An authored parameter or connection outside that list is an
`UnknownInput`. LOTUS-MATERIALX-04 does this for Standard Surface, whose
reader today reads its own list and nothing else; 05 and 08 start from
complete tables.

## 7. Tests

All but the last run without a device.

| Test | Tier | Fixes |
| --- | --- | --- |
| Node table | Hydra, no device | Every row of §5 from a hand-built network: the folded constant or lookup, component by component. Every exclusion: the diagnostic's kind, input and node |
| Compound equivalence | Hydra, no device | `ND_tiledimage`, `ND_gltf_image`, `ND_gltf_colorimage` and `ND_gltf_normalmap` each translate to the same IR as their library node graph written out in primitive nodes. The primitive graphs are transcribed from 1.39.5 into the test |
| Definitions | Hydra, no device | Each default and type the translator holds — the surfaces' and the nodes' — equals `SdrRegistry`'s for that identifier, and the three surface definitions' versions are [the pinned ones](MATERIAL_IR_V2.md#3-versions-contract-103). A runtime whose MaterialX moved fails here first |
| Completeness | Hydra, no device | Standard Surface: an authored value on every input of the definition yields a translation or a diagnostic, never neither |
| Stage round trip | Hydra, no device | A committed `.usda` and a `.mtlx` it references produce the same translation, so §3's table holds through UsdImaging |
| Graph fixture | Hydra and headless, device | `material-graphs-v1`, beside the existing `material-mtlx-v1`, which is unchanged |

`material-graphs-v1` renders Standard Surface through today's
`PreviewSurface` path, the only MaterialX surface with parameters until
05. Its materials fix what the roadmap's exit evidence names:

| Subject | Materials |
| --- | --- |
| Colour space | An sRGB constant against the same colour authored linear; `srgb_texture` against raw on one image |
| Channels | `separate3` of a packed image into roughness and metalness; `gltf_colorimage`'s `outcolor` with a `color` factor |
| Coordinates | `tiledimage` tiling and offset; `place2d` in both operation orders; `gltf_image` with a rotation, on an asymmetric image so that a flipped axis or a reversed order shows |
| Normal and tangent space | `gltf_normalmap` and `normalmap` of the same tilts image rendering the same card |
| Arithmetic | `multiply`, `add` and `mix` of an image with constants against the pre-multiplied image |
| Defaults | An unconnected `image` and an unconnected `mix` weight |

Pairs that must agree are compared with each other in the test, so the
fixture does not rest on its reference image alone. The headless mirror
builds the same IR by hand, which is the independent statement of what
each graph means.

## 8. Rejected alternatives in one place

| Alternative | Why not |
| --- | --- |
| A `.mtlx` reader in Lotus | A second input path with its own resolution rules, against the integration scope (§3). |
| Expanding compound nodes with the MaterialX library | Host-version behaviour and two more link dependencies, to save about ten rules (§3). |
| Approximating graphs that do not fold: baking to a texture, dropping the second lookup | Renders as if correct, which the contract forbids; baking is a feature of its own (§4). |
| An expression program for two-lookup graphs | [Already rejected](MATERIAL_IR_V2.md#42-inputs-and-texture-expressions) until a measured asset needs it; this document's diagnostics are how that asset will be found. |
| A visible error material | Loses the inputs that work (§6.2). |
| Extending `material-mtlx-v1` | Its committed references would change; a new fixture leaves them byte-identical. |

## 9. Acceptance for LOTUS-MATERIALX-04

1. The translator folds §5's nodes and no others, and §7's node-table and
   compound-equivalence tests pass.
2. Every unsupported surface, terminal, node, graph shape, value and
   authored input yields a diagnostic of §6.1's form, and the three silent
   behaviours of §2 are gone.
3. The definition test passes against the pinned runtime.
4. `material-graphs-v1` is committed with references under both
   integrators, and Hydra and headless agree on it.
5. Every existing committed reference and fixture is unchanged. Warnings
   that existing fixtures raise may change wording; a material that
   translated before and gains a diagnostic is listed in the report.
6. The scene reference documents the node table and the diagnostics, and
   the contract's §10.4 points here as settled.

## 10. Points settled in review

Accepted as proposed, 2026-11-15.

1. **No MaterialX link** (§3). The alternative is cheaper to extend and
   harder to pin.
2. **`constant` addressing with a non-zero default becomes a diagnostic**
   (§5.3), as does an unknown colour space (§5.5). Assets that rendered
   without a warning will now warn; none of their pixels change.
3. **`ND_tiledimage` tiling enters here.** The
   [deferred adapter work](../roadmap/current.md#deferred-adapter-work)
   holds it in Renderer Phase 8 until an asset needs it; every lookup
   already has an `st_transform`, and the compound rules need the same
   code, so it costs nothing extra to take now.
4. **Nodes left out.** `ND_ifgreater_*`, `ND_swizzle_*` (removed in 1.39),
   `ND_overlay_*`, `ND_ramp*`, `ND_hextiledimage_*`, colour transforms such
   as `ND_rgbtohsv_*`, and every procedural are diagnosed. Of the local
   survey assets, only toon-style Standard Surface graphs that multiply
   two images use the first three, and those need the expression program
   regardless.

## 11. Changes found in implementation

LOTUS-MATERIALX-04's implementation departs from or sharpens the text above
in these points. They were accepted in review, 2026-11-15, and supersede
the text they amend; the
[report](../reports/2026-11-15-materialx-graph-translation.md#changes-to-the-contract)
gives the evidence.

1. **`material-mtlx-v1`'s USD file changes by one node.** §7 and §9.5 keep
   the fixture unchanged, but its Untranslated card was an
   `ND_multiply_color3FA` of an image, which §5.2 now folds. The card's
   node is `ND_power_color3FA`, which does not fold, so the card, the
   mirror and the reference images stay byte-identical.
2. **Versions are not read through Sdr.** §7's definitions test compares
   every input, type and default through `SdrRegistry`, but Sdr carries no
   version for a MaterialX definition. The test reads the three surfaces'
   versions from the documents Sdr resolves their nodes to.
3. **`ND_gltf_colorimage` fixes its order and addressing.** Its library
   graph passes operation order 0 and periodic addressing to its image
   whatever the node's own inputs say; §5.3's rule follows the graph. And
   §7's normal-map pair is one lookup apart from its fallback:
   `ND_gltf_normalmap`'s `default` is the flat normal, `ND_image`'s black.
4. **OpenUSD's names for the two colour spaces.** Beside §5.5's table,
   `lin_rec709_scene` is raw and `srgb_rec709_scene` is sRGB.
5. **Smaller points.** §6.2's summary line groups `UnsupportedNode`,
   `UnsupportedGraph` and `UnsupportedValue`, whose outcome is one. §5.2's
   clamp is `base_color`'s or `emission_color`'s own only under a weight of
   1, since the IR clamps the product. A coloured `opacity` is
   `Substituted` by its mean. A connected `mix` weight that folds to a
   constant is a constant. An authored input that a node's definition does
   not name is `UnsupportedValue`.
