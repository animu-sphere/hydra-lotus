// SPDX-License-Identifier: Apache-2.0
// The MaterialX graph translation (docs/design/MATERIALX_TRANSLATION.md
// section 7), without a device. Hand-built Hydra networks fix every row of
// the node table and every exclusion's diagnostic; each compound node
// translates as its library graph written out in primitive nodes; the
// definitions the translator holds are the runtime's, read through Sdr;
// every Standard Surface input yields a translation or a diagnostic; and a
// stage's UsdShade network translates as the .mtlx document it references.
#include "adapter.hpp"
#include "material_translator.hpp"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

using Kind = HdLotusMaterialDiagnosticKind;
using Parameters = std::map<TfToken, VtValue>;

const std::filesystem::path kFixtures = LOTUS_MATERIAL_FIXTURES_DIRECTORY;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

Parameters P(std::initializer_list<std::pair<const char*, VtValue>> entries) {
  Parameters parameters;
  for (const auto& [name, value] : entries) parameters[TfToken(name)] = value;
  return parameters;
}

VtValue F(float value) {
  return VtValue(value);
}
VtValue V2(float x, float y) {
  return VtValue(GfVec2f(x, y));
}
VtValue V3(float x, float y, float z) {
  return VtValue(GfVec3f(x, y, z));
}
VtValue V4(float x, float y, float z, float w) {
  return VtValue(GfVec4f(x, y, z, w));
}
VtValue I(int value) {
  return VtValue(value);
}
VtValue S(const char* value) {
  return VtValue(std::string(value));
}
VtValue File(const char* path) {
  return VtValue(SdfAssetPath(path));
}

// One node of a hand-built MaterialX graph.
struct Node {
  std::string name;
  std::string id;
  Parameters parameters;
};

// A connection from node `from`'s output to node `to`'s input; "Surface"
// names the standard surface.
struct Link {
  std::string from;
  std::string output;
  std::string to;
  std::string input;
};

HdMaterialNetworkMap Network(Parameters surface, const std::vector<Node>& nodes = {},
    const std::vector<Link>& links = {},
    const char* surface_id = "ND_standard_surface_surfaceshader") {
  HdMaterialNetworkMap map;
  HdMaterialNetwork& network = map.map[HdMaterialTerminalTokens->surface];
  for (const Node& node : nodes) {
    HdMaterialNode added;
    added.path = SdfPath("/Material/" + node.name);
    added.identifier = TfToken(node.id);
    added.parameters = node.parameters;
    network.nodes.push_back(added);
  }
  HdMaterialNode terminal;
  terminal.path = SdfPath("/Material/Surface");
  terminal.identifier = TfToken(surface_id);
  terminal.parameters = std::move(surface);
  network.nodes.push_back(terminal);
  for (const Link& link : links) {
    network.relationships.push_back({SdfPath("/Material/" + link.from),
        TfToken(link.output), SdfPath("/Material/" + link.to), TfToken(link.input)});
  }
  map.terminals.push_back(terminal.path);
  return map;
}

HdLotusMaterialTranslation Translate(Parameters surface, const std::vector<Node>& nodes = {},
    const std::vector<Link>& links = {}) {
  return HdLotusTranslateMaterial(Network(std::move(surface), nodes, links));
}

// The same with MaterialX's glTF PBR as the surface.
HdLotusMaterialTranslation TranslateGltf(Parameters surface,
    const std::vector<Node>& nodes = {}, const std::vector<Link>& links = {}) {
  return HdLotusTranslateMaterial(
      Network(std::move(surface), nodes, links, "ND_gltf_pbr_surfaceshader"));
}

std::string Describe(const HdLotusMaterialTranslation& translation) {
  std::string text;
  for (const HdLotusMaterialDiagnostic& diagnostic : translation.diagnostics) {
    text += std::string(text.empty() ? "" : "; ") +
            HdLotusMaterialDiagnosticKindName(diagnostic.kind) + " on " +
            diagnostic.input.GetString() + " at " + diagnostic.node.GetString() + " (" +
            diagnostic.node_type.GetString() + "): " + diagnostic.detail;
  }
  return text.empty() ? "no diagnostics" : text;
}

std::string Describe(const Lotus::TextureInput& lookup) {
  const auto list = [](const auto& values) {
    std::string text;
    for (const float value : values) text += (text.empty() ? "" : " ") + std::to_string(value);
    return text;
  };
  return lookup.texture + " channel " + std::to_string(lookup.channel) + " scale " +
         list(lookup.scale) + " bias " + list(lookup.bias) + " fallback " +
         list(lookup.fallback) + " st " + list(lookup.st_transform) + " wraps " +
         std::to_string(static_cast<int>(lookup.wrap_s)) +
         std::to_string(static_cast<int>(lookup.wrap_t));
}

void Clean(const std::string& what, const HdLotusMaterialTranslation& translation) {
  Check(translation.diagnostics.empty(), what + ": " + Describe(translation));
}

// The translation's only diagnostic is of `kind`, on `input`, at node
// `node` of the hand-built network.
void Diagnosed(const std::string& what, const HdLotusMaterialTranslation& translation,
    Kind kind, const char* input, const char* node) {
  Check(translation.diagnostics.size() == 1 &&
            translation.diagnostics.front().kind == kind &&
            translation.diagnostics.front().input == TfToken(input) &&
            translation.diagnostics.front().node == SdfPath(std::string("/Material/") + node),
      what + ": expected one " + HdLotusMaterialDiagnosticKindName(kind) + " on " + input +
          " at " + node + ", got " + Describe(translation));
}

const Lotus::PreviewSurface& Surface(const HdLotusMaterialTranslation& translation) {
  return Lotus::Preview(translation.material);
}

void ExpectLookup(const std::string& what, const std::optional<Lotus::TextureInput>& actual,
    const Lotus::TextureInput& expected) {
  Check(actual.has_value(), what + ": no lookup");
  Check(*actual == expected,
      what + ": got " + Describe(*actual) + ", expected " + Describe(expected));
}

template <std::size_t N>
void ExpectValues(const std::string& what, const std::array<float, N>& actual,
    const std::array<float, N>& expected) {
  std::string got, want;
  for (std::size_t i = 0; i < N; ++i) {
    got += " " + std::to_string(actual[i]);
    want += " " + std::to_string(expected[i]);
  }
  Check(actual == expected, what + ": got" + got + ", expected" + want);
}

// An image lookup as MaterialX's defaults give it: periodic at st.
Lotus::TextureInput Lookup(const char* file, const char* space = "raw") {
  Lotus::TextureInput lookup;
  lookup.texture = HdLotusTextureKey(file, TfToken(space));
  lookup.wrap_s = lookup.wrap_t = Lotus::TextureWrap::Repeat;
  return lookup;
}

float Fold(double value) {
  return static_cast<float>(value);
}

const Node kColour{"Image", "ND_image_color3", P({{"file", File("colour.png")}})};
const Node kOther{"Other", "ND_image_color3", P({{"file", File("other.png")}})};
const Node kScalar{"Scalar", "ND_image_float", P({{"file", File("scalar.png")}})};
const Node kPacked{"Packed", "ND_image_vector3", P({{"file", File("packed.png")}})};
const Node kRgba{"Rgba", "ND_image_color4", P({{"file", File("rgba.png")}})};

// A colour graph ending at node `last` drives base_color under a base of 1.
HdLotusMaterialTranslation Colour(const std::vector<Node>& nodes, std::vector<Link> links,
    const char* last, const char* output = "out") {
  links.push_back({last, output, "Surface", "base_color"});
  return Translate({}, nodes, links);
}

// A scalar graph ending at node `last` drives specular_roughness.
HdLotusMaterialTranslation Scalar(const std::vector<Node>& nodes, std::vector<Link> links,
    const char* last, const char* output = "out") {
  links.push_back({last, output, "Surface", "specular_roughness"});
  return Translate({}, nodes, links);
}

void ValuesAndChannels() {
  // A constant node, and ND_dot passing a lookup through.
  HdLotusMaterialTranslation t = Colour(
      {{"Value", "ND_constant_color3", P({{"value", V3(0.1F, 0.2F, 0.3F)}})}}, {}, "Value");
  Clean("ND_constant", t);
  ExpectValues("ND_constant", Surface(t).base_color, {0.1F, 0.2F, 0.3F});
  t = Colour({kColour, {"Dot", "ND_dot_color3", P({{"note", S("pass")}})}},
      {{"Image", "out", "Dot", "in"}}, "Dot");
  Clean("ND_dot", t);
  ExpectLookup("ND_dot", Surface(t).base_color_texture, Lookup("colour.png"));

  // ND_convert: a scalar spreads, a wider type gains 0 and then 1, a
  // narrower one keeps its first components.
  t = Colour({{"Spread", "ND_convert_float_color3", P({{"in", F(0.25F)}})}}, {}, "Spread");
  Clean("ND_convert of a scalar", t);
  ExpectValues("ND_convert of a scalar", Surface(t).base_color, {0.25F, 0.25F, 0.25F});
  t = Colour({{"Wider", "ND_convert_vector2_color3", P({{"in", V2(0.5F, 0.75F)}})}}, {},
      "Wider");
  Clean("ND_convert of a vector2", t);
  ExpectValues("ND_convert of a vector2", Surface(t).base_color, {0.5F, 0.75F, 0.0F});
  t = Scalar({{"Wider", "ND_convert_color3_color4", P({{"in", V3(0.1F, 0.2F, 0.3F)}})},
                 {"Fourth", "ND_extract_color4", P({{"index", I(3)}})}},
      {{"Wider", "out", "Fourth", "in"}}, "Fourth");
  Clean("ND_convert to four components", t);
  Check(Surface(t).roughness == 1.0F, "a converted colour's fourth component is not 1");
  t = Colour({{"Narrower", "ND_convert_color4_color3",
                 P({{"in", V4(0.1F, 0.2F, 0.3F, 0.4F)}})}},
      {}, "Narrower");
  ExpectValues("ND_convert to three components", Surface(t).base_color, {0.1F, 0.2F, 0.3F});
  // A lookup converts between three and four components; its fourth is then
  // 1 whatever the image holds.
  t = Scalar({kColour, {"Wider", "ND_convert_color3_color4", {}},
                 {"Fourth", "ND_extract_color4", P({{"index", I(3)}})}},
      {{"Image", "out", "Wider", "in"}, {"Wider", "out", "Fourth", "in"}}, "Fourth");
  Clean("ND_convert of a lookup", t);
  Lotus::TextureInput expected = Lookup("colour.png");
  expected.channel = 3;
  expected.scale[3] = 0.0F;
  expected.bias[3] = 1.0F;
  ExpectLookup("ND_convert of a lookup", Surface(t).roughness_texture, expected);
  t = Colour({kScalar, {"Spread", "ND_convert_float_color3", {}}},
      {{"Scalar", "out", "Spread", "in"}}, "Spread");
  Diagnosed("ND_convert of a scalar lookup", t, Kind::UnsupportedGraph, "base_color", "Spread");
  ExpectValues("a diagnosed input's default", Surface(t).base_color, {0.8F, 0.8F, 0.8F});
  Check(!Surface(t).base_color_texture && t.textures.empty(),
      "a diagnosed input kept a lookup");

  // ND_extract and ND_separate: a constant's component, a lookup's channel.
  t = Scalar({{"Pick", "ND_extract_vector3", P({{"in", V3(0.1F, 0.2F, 0.3F)}, {"index", I(1)}})}},
      {}, "Pick");
  Clean("ND_extract of a constant", t);
  Check(Surface(t).roughness == 0.2F, "ND_extract did not take the constant's component");
  t = Scalar({kPacked, {"Pick", "ND_extract_vector3", P({{"index", I(2)}})}},
      {{"Packed", "out", "Pick", "in"}}, "Pick");
  expected = Lookup("packed.png");
  expected.channel = 2;
  ExpectLookup("ND_extract of a lookup", Surface(t).roughness_texture, expected);
  t = Scalar({kPacked, {"Pick", "ND_extract_vector3", P({{"index", I(3)}})}},
      {{"Packed", "out", "Pick", "in"}}, "Pick");
  Diagnosed("ND_extract beyond the type", t, Kind::UnsupportedValue, "specular_roughness",
      "Pick");
  t = Scalar({kPacked, {"Pick", "ND_extract_vector3", {}},
                 {"Index", "ND_constant_float", {}}},
      {{"Packed", "out", "Pick", "in"}, {"Index", "out", "Pick", "index"}}, "Pick");
  Diagnosed("a connected index", t, Kind::UnsupportedGraph, "specular_roughness", "Pick");
  for (const auto& [id, output, channel] :
      {std::tuple{"ND_separate3_vector3", "outy", 1U}, std::tuple{"ND_separate3_vector3", "outz", 2U}}) {
    t = Scalar({kPacked, {"Split", id, {}}}, {{"Packed", "out", "Split", "in"}}, "Split",
        output);
    Clean("ND_separate3", t);
    expected = Lookup("packed.png");
    expected.channel = channel;
    ExpectLookup("ND_separate3", Surface(t).roughness_texture, expected);
  }
  t = Scalar({kRgba, {"Split", "ND_separate4_color4", {}}}, {{"Rgba", "out", "Split", "in"}},
      "Split", "outa");
  expected = Lookup("rgba.png");
  expected.channel = 3;
  expected.fallback = {0.0F, 0.0F, 0.0F, 0.0F};
  ExpectLookup("ND_separate4", Surface(t).roughness_texture, expected);
  t = Scalar({{"Split", "ND_separate2_vector2", P({{"in", V2(0.25F, 0.75F)}})}}, {}, "Split",
      "outy");
  Check(Surface(t).roughness == 0.75F, "ND_separate2 did not take the constant's component");
  t = Scalar({kPacked, {"Split", "ND_separate3_vector3", {}}},
      {{"Packed", "out", "Split", "in"}}, "Split", "outw");
  Diagnosed("ND_separate3's missing output", t, Kind::UnsupportedGraph, "specular_roughness",
      "Split");

  // ND_combine: constants, or one lookup's channels in order, each with its
  // own scale and bias.
  t = Colour({{"Join", "ND_combine3_color3",
                 P({{"in1", F(0.1F)}, {"in2", F(0.2F)}, {"in3", F(0.3F)}})}},
      {}, "Join");
  Clean("ND_combine3 of constants", t);
  ExpectValues("ND_combine3 of constants", Surface(t).base_color, {0.1F, 0.2F, 0.3F});
  t = Scalar({{"Join", "ND_combine2_color4CF", P({{"in1", V3(0.1F, 0.2F, 0.3F)}, {"in2", F(0.4F)}})},
                 {"Fourth", "ND_extract_color4", P({{"index", I(3)}})}},
      {{"Join", "out", "Fourth", "in"}}, "Fourth");
  Check(Surface(t).roughness == 0.4F, "ND_combine2 did not append its scalar");
  const std::vector<Node> split{kColour, {"Split", "ND_separate3_color3", {}},
      {"Double", "ND_multiply_float", P({{"in2", F(2.0F)}})},
      {"Lift", "ND_add_float", P({{"in2", F(0.25F)}})},
      {"Join", "ND_combine3_color3", {}}};
  t = Colour(split,
      {{"Image", "out", "Split", "in"}, {"Split", "outr", "Join", "in1"},
          {"Split", "outg", "Double", "in1"}, {"Double", "out", "Join", "in2"},
          {"Split", "outb", "Lift", "in1"}, {"Lift", "out", "Join", "in3"}},
      "Join");
  Clean("ND_combine3 of a lookup", t);
  expected = Lookup("colour.png");
  expected.scale = {1.0F, 2.0F, 1.0F, 1.0F};
  expected.bias = {0.0F, 0.0F, 0.25F, 0.0F};
  expected.fallback = {0.0F, 0.0F, 0.25F, 1.0F};
  ExpectLookup("ND_combine3 of a lookup", Surface(t).base_color_texture, expected);
  t = Colour(split,
      {{"Image", "out", "Split", "in"}, {"Split", "outg", "Join", "in1"},
          {"Split", "outr", "Join", "in2"}, {"Split", "outb", "Join", "in3"}},
      "Join");
  Diagnosed("ND_combine3 out of order", t, Kind::UnsupportedGraph, "base_color", "Join");
  t = Colour(split,
      {{"Image", "out", "Split", "in"}, {"Split", "outr", "Join", "in1"},
          {"Split", "outg", "Join", "in2"}},
      "Join");
  Diagnosed("ND_combine3 of a lookup and a constant", t, Kind::UnsupportedGraph, "base_color",
      "Join");
}

void Arithmetic() {
  // Constants fold in double precision.
  struct Folded {
    const char* id;
    float a, b;
    double expected;
  };
  for (const Folded& fold : std::initializer_list<Folded>{
           {"ND_add_float", 0.1F, 0.2F, double{0.1F} + double{0.2F}},
           {"ND_subtract_float", 0.9F, 0.2F, double{0.9F} - double{0.2F}},
           {"ND_multiply_float", 0.3F, 0.7F, double{0.3F} * double{0.7F}},
           {"ND_divide_float", 0.3F, 0.7F, double{0.3F} / double{0.7F}},
           {"ND_min_float", 0.3F, 0.7F, 0.3F},
           {"ND_max_float", 0.3F, 0.7F, 0.7F},
           {"ND_power_float", 0.3F, 0.7F, std::pow(double{0.3F}, double{0.7F})}}) {
    const HdLotusMaterialTranslation t =
        Scalar({{"Fold", fold.id, P({{"in1", F(fold.a)}, {"in2", F(fold.b)}})}}, {}, "Fold");
    Clean(fold.id, t);
    Check(Surface(t).roughness == Fold(fold.expected),
        std::string(fold.id) + " folded to " + std::to_string(Surface(t).roughness));
  }
  for (const auto& [id, in, expected] :
      {std::tuple{"ND_absval_float", -0.25F, 0.25}, std::tuple{"ND_sqrt_float", 0.25F, 0.5},
          std::tuple{"ND_floor_float", 0.75F, 0.0}, std::tuple{"ND_ceil_float", 0.25F, 1.0}}) {
    const HdLotusMaterialTranslation t =
        Scalar({{"Fold", id, P({{"in", F(in)}})}}, {}, "Fold");
    Clean(id, t);
    Check(Surface(t).roughness == Fold(expected), std::string(id) + " did not fold");
  }
  HdLotusMaterialTranslation t =
      Scalar({{"Fold", "ND_divide_float", P({{"in1", F(0.5F)}, {"in2", F(0.0F)}})}}, {}, "Fold");
  Diagnosed("a constant divided by zero", t, Kind::UnsupportedValue, "specular_roughness",
      "Fold");
  Check(Surface(t).roughness == 0.2F, "a diagnosed roughness left its default");
  t = Colour({{"Scale", "ND_multiply_color3FA", P({{"in1", V3(0.2F, 0.4F, 0.6F)}, {"in2", F(0.5F)}})}},
      {}, "Scale");
  ExpectValues("a colour times a scalar", Surface(t).base_color,
      {Fold(double{0.2F} * 0.5), Fold(double{0.4F} * 0.5), Fold(double{0.6F} * 0.5)});

  // With one lookup: the other side moves the bias or scales.
  const auto binary = [](const char* id, bool lookup_first, VtValue constant) {
    return Colour({kColour, {"Op", id, P({{lookup_first ? "in2" : "in1", constant}})}},
        {{"Image", "out", "Op", lookup_first ? "in1" : "in2"}}, "Op");
  };
  Lotus::TextureInput expected = Lookup("colour.png");
  expected.bias = {0.1F, 0.2F, 0.3F, 0.0F};
  expected.fallback = {0.1F, 0.2F, 0.3F, 1.0F};
  t = binary("ND_add_color3", true, V3(0.1F, 0.2F, 0.3F));
  Clean("a lookup plus a constant", t);
  ExpectLookup("a lookup plus a constant", Surface(t).base_color_texture, expected);
  t = binary("ND_add_color3", false, V3(0.1F, 0.2F, 0.3F));
  ExpectLookup("a constant plus a lookup", Surface(t).base_color_texture, expected);
  expected.bias = {-0.1F, -0.2F, -0.3F, 0.0F};
  expected.fallback = {-0.1F, -0.2F, -0.3F, 1.0F};
  t = binary("ND_subtract_color3", true, V3(0.1F, 0.2F, 0.3F));
  ExpectLookup("a lookup minus a constant", Surface(t).base_color_texture, expected);
  expected.scale = {-1.0F, -1.0F, -1.0F, 1.0F};
  expected.bias = {0.1F, 0.2F, 0.3F, 0.0F};
  expected.fallback = {0.1F, 0.2F, 0.3F, 1.0F};
  t = binary("ND_subtract_color3", false, V3(0.1F, 0.2F, 0.3F));
  Clean("a constant minus a lookup", t);
  ExpectLookup("a constant minus a lookup", Surface(t).base_color_texture, expected);
  t = Colour({kColour, {"Op", "ND_invert_color3", P({{"amount", V3(0.1F, 0.2F, 0.3F)}})}},
      {{"Image", "out", "Op", "in"}}, "Op");
  Clean("ND_invert", t);
  ExpectLookup("ND_invert", Surface(t).base_color_texture, expected);
  expected = Lookup("colour.png");
  expected.scale = {0.5F, 0.25F, 2.0F, 1.0F};
  t = binary("ND_multiply_color3", false, V3(0.5F, 0.25F, 2.0F));
  ExpectLookup("a constant times a lookup", Surface(t).base_color_texture, expected);
  t = binary("ND_divide_color3", true, V3(2.0F, 4.0F, 0.5F));
  Clean("a lookup divided by a constant", t);
  ExpectLookup("a lookup divided by a constant", Surface(t).base_color_texture, expected);
  expected.scale = {0.5F, 0.5F, 0.5F, 1.0F};
  t = binary("ND_multiply_color3FA", true, F(0.5F));
  ExpectLookup("a lookup times a scalar", Surface(t).base_color_texture, expected);
  // A chain keeps one scale and one bias.
  t = Colour({kColour, {"Half", "ND_multiply_color3FA", P({{"in2", F(0.5F)}})},
                 {"Lift", "ND_add_color3FA", P({{"in2", F(0.25F)}})},
                 {"Twice", "ND_multiply_color3FA", P({{"in2", F(2.0F)}})}},
      {{"Image", "out", "Half", "in1"}, {"Half", "out", "Lift", "in1"},
          {"Lift", "out", "Twice", "in1"}},
      "Twice");
  expected = Lookup("colour.png");
  expected.bias = {0.5F, 0.5F, 0.5F, 0.0F};
  expected.fallback = {0.5F, 0.5F, 0.5F, 1.0F};
  Clean("a chain", t);
  ExpectLookup("a chain", Surface(t).base_color_texture, expected);

  t = binary("ND_divide_color3", true, V3(2.0F, 0.0F, 0.5F));
  Diagnosed("a lookup divided by zero", t, Kind::UnsupportedValue, "base_color", "Op");
  t = binary("ND_divide_color3", false, V3(1.0F, 1.0F, 1.0F));
  Diagnosed("a constant divided by a lookup", t, Kind::UnsupportedGraph, "base_color", "Op");
  for (const char* id : {"ND_min_color3", "ND_max_color3", "ND_power_color3"}) {
    t = binary(id, true, V3(0.5F, 0.5F, 0.5F));
    Diagnosed(id, t, Kind::UnsupportedGraph, "base_color", "Op");
  }
  t = Colour({kColour, {"Op", "ND_absval_color3", {}}}, {{"Image", "out", "Op", "in"}}, "Op");
  Diagnosed("ND_absval of a lookup", t, Kind::UnsupportedGraph, "base_color", "Op");
  t = Colour({kColour, kOther, {"Op", "ND_multiply_color3", {}}},
      {{"Image", "out", "Op", "in1"}, {"Other", "out", "Op", "in2"}}, "Op");
  Diagnosed("two lookups", t, Kind::UnsupportedGraph, "base_color", "Op");

  // ND_mix: bg (1 - mix) + fg mix with a constant mix.
  t = Colour({{"Mix", "ND_mix_color3",
                 P({{"fg", V3(1.0F, 0.5F, 0.0F)}, {"bg", V3(0.0F, 0.5F, 1.0F)},
                     {"mix", F(0.25F)}})}},
      {}, "Mix");
  Clean("ND_mix of constants", t);
  ExpectValues("ND_mix of constants", Surface(t).base_color, {0.25F, 0.5F, 0.75F});
  const auto mix = [](const char* lookup, Parameters parameters) {
    return Colour({kColour, {"Mix", "ND_mix_color3", std::move(parameters)}},
        {{"Image", "out", "Mix", lookup}}, "Mix");
  };
  t = mix("fg", P({{"bg", V3(0.4F, 0.8F, 0.0F)}, {"mix", F(0.25F)}}));
  expected = Lookup("colour.png");
  expected.scale = {0.25F, 0.25F, 0.25F, 1.0F};
  expected.bias = {Fold(double{0.4F} * 0.75), Fold(double{0.8F} * 0.75), 0.0F, 0.0F};
  expected.fallback = {expected.bias[0], expected.bias[1], 0.0F, 1.0F};
  Clean("ND_mix of a foreground lookup", t);
  ExpectLookup("ND_mix of a foreground lookup", Surface(t).base_color_texture, expected);
  t = mix("bg", P({{"fg", V3(0.4F, 0.8F, 0.0F)}, {"mix", F(0.25F)}}));
  expected.scale = {0.75F, 0.75F, 0.75F, 1.0F};
  expected.bias = {Fold(double{0.4F} * 0.25), Fold(double{0.8F} * 0.25), 0.0F, 0.0F};
  expected.fallback = {expected.bias[0], expected.bias[1], 0.0F, 1.0F};
  ExpectLookup("ND_mix of a background lookup", Surface(t).base_color_texture, expected);
  // An unconnected mix weight is 0: the background.
  t = mix("fg", P({{"bg", V3(0.4F, 0.8F, 0.0F)}}));
  expected.scale = {0.0F, 0.0F, 0.0F, 1.0F};
  expected.bias = {0.4F, 0.8F, 0.0F, 0.0F};
  expected.fallback = {0.4F, 0.8F, 0.0F, 1.0F};
  Clean("ND_mix without a weight", t);
  ExpectLookup("ND_mix without a weight", Surface(t).base_color_texture, expected);
  t = Colour({kColour, kScalar, {"Mix", "ND_mix_color3", {}}},
      {{"Image", "out", "Mix", "fg"}, {"Scalar", "out", "Mix", "mix"}}, "Mix");
  Diagnosed("a mix weight from a lookup", t, Kind::UnsupportedGraph, "base_color", "Mix");
  t = Colour({kColour, kOther, {"Mix", "ND_mix_color3", P({{"mix", F(0.5F)}})}},
      {{"Image", "out", "Mix", "fg"}, {"Other", "out", "Mix", "bg"}}, "Mix");
  Diagnosed("a mix of two lookups", t, Kind::UnsupportedGraph, "base_color", "Mix");

  // ND_clamp: constants fold; a lookup's passes only as the last node
  // before an input whose own range lies inside it.
  t = Colour({{"Clamp", "ND_clamp_color3",
                 P({{"in", V3(-0.5F, 0.5F, 1.5F)}, {"low", V3(0.25F, 0.25F, 0.25F)},
                     {"high", V3(0.75F, 0.75F, 0.75F)}})}},
      {}, "Clamp");
  Clean("ND_clamp of a constant", t);
  ExpectValues("ND_clamp of a constant", Surface(t).base_color, {0.25F, 0.5F, 0.75F});
  const auto clamp = [](Parameters surface, Parameters parameters) {
    return Translate(std::move(surface), {kColour, {"Clamp", "ND_clamp_color3", std::move(parameters)}},
        {{"Image", "out", "Clamp", "in"}, {"Clamp", "out", "Surface", "base_color"}});
  };
  t = clamp({}, {});
  Clean("a clamp to the input's range", t);
  ExpectLookup("a clamp to the input's range", Surface(t).base_color_texture,
      Lookup("colour.png"));
  t = clamp({}, P({{"low", V3(-1.0F, -1.0F, -1.0F)}, {"high", V3(2.0F, 2.0F, 2.0F)}}));
  Clean("a clamp wider than the input's range", t);
  t = clamp({}, P({{"high", V3(1.0F, 0.5F, 1.0F)}}));
  Diagnosed("a clamp narrower than the input's range", t, Kind::UnsupportedGraph, "base_color",
      "Clamp");
  // Under a base weight the clamp is no longer the input's own.
  t = clamp(P({{"base", F(0.5F)}}), {});
  Diagnosed("a clamp under a weight", t, Kind::UnsupportedGraph, "base_color", "Clamp");
  t = Colour({kColour, {"Clamp", "ND_clamp_color3", {}},
                 {"Half", "ND_multiply_color3FA", P({{"in2", F(0.5F)}})}},
      {{"Image", "out", "Clamp", "in"}, {"Clamp", "out", "Half", "in1"}}, "Half");
  Diagnosed("a clamp that is not the last node", t, Kind::UnsupportedGraph, "base_color",
      "Clamp");
}

// The coordinate transform the translation gave base_color's lookup.
std::array<float, 6> St(const HdLotusMaterialTranslation& translation) {
  Check(Surface(translation).base_color_texture.has_value(),
      "no base colour lookup: " + Describe(translation));
  return Surface(translation).base_color_texture->st_transform;
}

void ImagesAndCoordinates() {
  // ND_image: address modes, the default as fallback, and coordinates.
  HdLotusMaterialTranslation t = Colour(
      {{"Image", "ND_image_color3",
          P({{"file", File("colour.png")}, {"default", V3(0.1F, 0.2F, 0.3F)},
              {"uaddressmode", S("clamp")}, {"vaddressmode", S("mirror")},
              {"filtertype", S("linear")}, {"layer", S("")}, {"framerange", S("")},
              {"frameoffset", I(3)}, {"frameendaction", S("constant")}})}},
      {}, "Image");
  Clean("ND_image", t);
  Lotus::TextureInput expected = Lookup("colour.png");
  expected.wrap_s = Lotus::TextureWrap::Clamp;
  expected.wrap_t = Lotus::TextureWrap::Mirror;
  expected.fallback = {0.1F, 0.2F, 0.3F, 1.0F};
  ExpectLookup("ND_image", Surface(t).base_color_texture, expected);
  Check(t.material.geometry.texcoords == "st" && t.textures.size() == 1 &&
            t.textures.front().file == "colour.png",
      "ND_image did not read at st or request its file");
  t = Colour({{"Image", "ND_image_color3", P({{"default", V3(0.1F, 0.2F, 0.3F)}})}}, {}, "Image");
  Clean("ND_image without a file", t);
  ExpectValues("ND_image without a file", Surface(t).base_color, {0.1F, 0.2F, 0.3F});
  t = Colour({{"Image", "ND_image_color3",
                 P({{"file", File("colour.png")}, {"texcoord", V2(0.25F, 0.75F)}})}},
      {}, "Image");
  Clean("ND_image at a constant point", t);
  Check(t.material.geometry.texcoords.empty() &&
            t.material.geometry.texcoord_fallback == std::array<float, 2>{0.25F, 0.75F},
      "a constant texcoord did not become the material's point");
  t = Colour({{"Image", "ND_image_color3",
                 P({{"file", File("colour.png")}, {"uaddressmode", S("constant")},
                     {"vaddressmode", S("constant")}})}},
      {}, "Image");
  Clean("constant addressing with a zero default", t);
  Check(Surface(t).base_color_texture->wrap_s == Lotus::TextureWrap::Black,
      "constant addressing is not black");

  // Substitutions translate, and say so.
  const auto image = [](Parameters parameters) {
    parameters[TfToken("file")] = File("colour.png");
    return Colour({{"Image", "ND_image_color3", std::move(parameters)}}, {}, "Image");
  };
  t = image(P({{"uaddressmode", S("constant")}, {"default", V3(0.5F, 0.0F, 0.0F)}}));
  Diagnosed("constant addressing with a default", t, Kind::Substituted, "base_color", "Image");
  Check(Surface(t).base_color_texture.has_value(), "a substituted input lost its lookup");
  for (const char* filter : {"closest", "cubic"}) {
    t = image(P({{"filtertype", S(filter)}}));
    Diagnosed(std::string("filtertype ") + filter, t, Kind::Substituted, "base_color", "Image");
    Check(Surface(t).base_color_texture.has_value(), "a substituted input lost its lookup");
  }
  // Values outside a rule leave the default.
  for (const auto& [name, value] :
      {std::pair{"layer", S("diffuse")}, std::pair{"framerange", S("1-10")},
          std::pair{"uaddressmode", S("wrap")}, std::pair{"filtertype", S("nearest")},
          std::pair{"blur", S("1")}, std::pair{"default", S("grey")}}) {
    t = image(P({{name, value}}));
    Diagnosed(std::string("ND_image ") + name, t, Kind::UnsupportedValue, "base_color", "Image");
    Check(!Surface(t).base_color_texture, "an unsupported value kept its lookup");
  }

  // Colour spaces: linear and raw names are raw, srgb_texture decodes, and
  // another is read as raw with a diagnostic. A float image has none.
  for (const auto& [space, key, clean] :
      {std::tuple{"lin_rec709", "raw", true}, std::tuple{"raw", "raw", true},
          std::tuple{"data", "raw", true}, std::tuple{"none", "raw", true},
          std::tuple{"srgb_texture", "sRGB", true}, std::tuple{"acescg", "raw", false},
          std::tuple{"g22_rec709", "raw", false}, std::tuple{"srgb_displayp3", "raw", false}}) {
    t = image(P({{"colorSpace:file", VtValue(TfToken(space))}}));
    if (clean) {
      Clean(std::string("colour space ") + space, t);
    } else {
      Diagnosed(std::string("colour space ") + space, t, Kind::Substituted, "base_color",
          "Image");
    }
    ExpectLookup(std::string("colour space ") + space, Surface(t).base_color_texture,
        Lookup("colour.png", key));
  }
  t = Scalar({{"Scalar", "ND_image_float",
                 P({{"file", File("scalar.png")},
                     {"colorSpace:file", VtValue(TfToken("srgb_texture"))}})}},
      {}, "Scalar");
  Clean("a float image's colour space", t);
  ExpectLookup("a float image's colour space", Surface(t).roughness_texture,
      Lookup("scalar.png"));
  // A constant colour: decoded when folded, on the surface or in a node.
  const std::array<float, 3> decoded{
      Fold(std::pow((double{0.8F} + 0.055) / 1.055, 2.4)),
      Fold(std::pow((double{0.4F} + 0.055) / 1.055, 2.4)), Fold(double{0.03F} / 12.92)};
  t = Translate(P({{"base_color", V3(0.8F, 0.4F, 0.03F)},
      {"colorSpace:base_color", VtValue(TfToken("srgb_texture"))}}));
  Clean("an sRGB constant", t);
  ExpectValues("an sRGB constant", Surface(t).base_color, decoded);
  t = Colour({{"Value", "ND_constant_color3",
                 P({{"value", V3(0.8F, 0.4F, 0.03F)},
                     {"colorSpace:value", VtValue(TfToken("srgb_texture"))}})}},
      {}, "Value");
  ExpectValues("an sRGB constant in a node", Surface(t).base_color, decoded);
  t = Translate(P({{"base_color", V3(0.8F, 0.4F, 0.03F)},
      {"colorSpace:base_color", VtValue(TfToken("acescg"))}}));
  Diagnosed("a constant in another colour space", t, Kind::Substituted, "base_color",
      "Surface");
  ExpectValues("a constant in another colour space", Surface(t).base_color,
      {0.8F, 0.4F, 0.03F});

  // ND_tiledimage: periodic at uv x uvtiling - uvoffset.
  t = Colour({{"Image", "ND_tiledimage_color3",
                 P({{"file", File("colour.png")}, {"uvtiling", V2(2.0F, 3.0F)},
                     {"uvoffset", V2(0.25F, 0.5F)}})}},
      {}, "Image");
  Clean("ND_tiledimage", t);
  ExpectValues("ND_tiledimage", St(t), {2.0F, 0.0F, -0.25F, 0.0F, 3.0F, -0.5F});
  Check(Surface(t).base_color_texture->wrap_s == Lotus::TextureWrap::Repeat,
      "ND_tiledimage is not periodic");
  t = Colour({{"Image", "ND_tiledimage_color3",
                 P({{"file", File("colour.png")}, {"realworldimagesize", V2(2.0F, 2.0F)}})}},
      {}, "Image");
  Diagnosed("real-world sizes", t, Kind::UnsupportedValue, "base_color", "Image");

  // Coordinate sources.
  const auto at = [](std::vector<Node> nodes, std::vector<Link> links, const char* last) {
    nodes.push_back(kColour);
    links.push_back({last, "out", "Image", "texcoord"});
    return Colour(nodes, links, "Image");
  };
  t = at({{"Coords", "ND_texcoord_vector2", {}}}, {}, "Coords");
  Clean("ND_texcoord", t);
  Check(t.material.geometry.texcoords == "st", "ND_texcoord is not st");
  t = at({{"Coords", "ND_texcoord_vector2", P({{"index", I(1)}})}}, {}, "Coords");
  Diagnosed("a second texcoord index", t, Kind::UnsupportedValue, "base_color", "Coords");
  t = at({{"Coords", "ND_geompropvalue_vector2",
             P({{"geomprop", S("uv1")}, {"default", V2(0.5F, 0.25F)}})}},
      {}, "Coords");
  Clean("ND_geompropvalue", t);
  Check(t.material.geometry.texcoords == "uv1" &&
            t.material.geometry.texcoord_fallback == std::array<float, 2>{0.5F, 0.25F},
      "ND_geompropvalue did not name its set and fallback");
  t = at({kOther}, {}, "Other");
  Check(t.diagnostics.size() == 1 && t.diagnostics.front().kind == Kind::UnsupportedGraph,
      "coordinates from an image: " + Describe(t));

  // Arithmetic on coordinates composes one transform.
  const Node coords{"Coords", "ND_texcoord_vector2", {}};
  const auto mapped = [&](const char* id, Parameters parameters, const char* input = "in1") {
    return at({coords, {"Map", id, std::move(parameters)}}, {{"Coords", "out", "Map", input}},
        "Map");
  };
  t = mapped("ND_multiply_vector2", P({{"in2", V2(2.0F, 4.0F)}}));
  Clean("coordinates times a constant", t);
  ExpectValues("coordinates times a constant", St(t), {2.0F, 0.0F, 0.0F, 0.0F, 4.0F, 0.0F});
  t = mapped("ND_divide_vector2", P({{"in2", V2(2.0F, 4.0F)}}));
  ExpectValues("coordinates over a constant", St(t), {0.5F, 0.0F, 0.0F, 0.0F, 0.25F, 0.0F});
  t = mapped("ND_add_vector2", P({{"in2", V2(0.25F, 0.5F)}}));
  ExpectValues("coordinates plus a constant", St(t), {1.0F, 0.0F, 0.25F, 0.0F, 1.0F, 0.5F});
  t = mapped("ND_subtract_vector2", P({{"in2", V2(0.25F, 0.5F)}}));
  ExpectValues("coordinates minus a constant", St(t), {1.0F, 0.0F, -0.25F, 0.0F, 1.0F, -0.5F});
  t = mapped("ND_subtract_vector2", P({{"in1", V2(1.0F, 1.0F)}}), "in2");
  Clean("a constant minus coordinates", t);
  ExpectValues("a constant minus coordinates", St(t), {-1.0F, 0.0F, 1.0F, 0.0F, -1.0F, 1.0F});
  t = mapped("ND_multiply_vector2FA", P({{"in2", F(3.0F)}}));
  ExpectValues("coordinates times a scalar", St(t), {3.0F, 0.0F, 0.0F, 0.0F, 3.0F, 0.0F});
  t = mapped("ND_divide_vector2", P({{"in1", V2(1.0F, 1.0F)}}), "in2");
  Diagnosed("a constant over coordinates", t, Kind::UnsupportedGraph, "base_color", "Map");
  t = mapped("ND_divide_vector2", P({{"in2", V2(2.0F, 0.0F)}}));
  Diagnosed("coordinates over zero", t, Kind::UnsupportedValue, "base_color", "Map");
  // ND_rotate2d turns (x, y) into (c x + s y, -s x + c y).
  const double c = std::cos(30.0 * 3.14159265358979323846 / 180.0);
  const double s = std::sin(30.0 * 3.14159265358979323846 / 180.0);
  t = mapped("ND_rotate2d_vector2", P({{"amount", F(30.0F)}}), "in");
  Clean("ND_rotate2d", t);
  ExpectValues("ND_rotate2d", St(t), {Fold(c), Fold(s), 0.0F, Fold(-s), Fold(c), 0.0F});

  // A material reads one coordinate set.
  t = Translate({},
      {{"Coords", "ND_geompropvalue_vector2", P({{"geomprop", S("uv1")}})}, kColour, kScalar},
      {{"Coords", "out", "Scalar", "texcoord"}, {"Image", "out", "Surface", "base_color"},
          {"Scalar", "out", "Surface", "metalness"}});
  Diagnosed("a second coordinate set", t, Kind::UnsupportedGraph, "metalness", "Scalar");
  Check(!Surface(t).metallic_texture && t.material.geometry.texcoords == "st",
      "a second coordinate set was taken");
}

void Normals() {
  const Node bumps{"Bumps", "ND_image_vector3", P({{"file", File("normal.png")}})};
  const auto normal = [&](const char* id, Parameters parameters) {
    return Translate({}, {bumps, {"Normal", id, std::move(parameters)}},
        {{"Bumps", "out", "Normal", "in"}, {"Normal", "out", "Surface", "normal"}});
  };
  HdLotusMaterialTranslation t = normal("ND_normalmap_float", P({{"scale", F(0.5F)}}));
  Clean("ND_normalmap_float", t);
  Lotus::TextureInput expected = Lookup("normal.png");
  expected.scale = {1.0F, 1.0F, 2.0F, 1.0F};
  expected.bias = {-0.5F, -0.5F, -1.0F, 0.0F};
  expected.fallback = {-0.5F, -0.5F, -1.0F, 1.0F};
  ExpectLookup("ND_normalmap_float", t.material.geometry.normal_texture, expected);
  t = normal("ND_normalmap_vector2", P({{"scale", V2(0.5F, 0.25F)}}));
  expected.scale = {1.0F, 0.5F, 2.0F, 1.0F};
  expected.bias = {-0.5F, -0.25F, -1.0F, 0.0F};
  expected.fallback = {-0.5F, -0.25F, -1.0F, 1.0F};
  ExpectLookup("ND_normalmap_vector2", t.material.geometry.normal_texture, expected);
  // Without an image, the constant it decodes.
  t = Translate({}, {{"Normal", "ND_normalmap_float", P({{"in", V3(0.75F, 0.25F, 1.0F)}})}},
      {{"Normal", "out", "Surface", "normal"}});
  Clean("a constant normal map", t);
  ExpectValues("a constant normal map", t.material.geometry.normal, {0.5F, -0.5F, 1.0F});
  for (const char* frame : {"normal", "tangent", "bitangent"}) {
    t = normal("ND_normalmap_float", P({{frame, V3(0.0F, 0.0F, 1.0F)}}));
    Diagnosed(std::string("an authored ") + frame, t, Kind::UnsupportedGraph, "normal", "Normal");
    Check(!t.material.geometry.normal_texture, "a diagnosed normal kept its lookup");
  }
  // A normal map feeds only a normal; a colour graph does not.
  t = Translate({}, {bumps, {"Normal", "ND_normalmap_float", {}}},
      {{"Bumps", "out", "Normal", "in"}, {"Normal", "out", "Surface", "base_color"}});
  Diagnosed("a normal map on a colour", t, Kind::UnsupportedGraph, "base_color", "Normal");
  ExpectValues("a normal map on a colour", Surface(t).base_color, {0.8F, 0.8F, 0.8F});
  t = Translate({}, {bumps}, {{"Bumps", "out", "Surface", "normal"}});
  Diagnosed("an image on the normal", t, Kind::UnsupportedGraph, "normal", "Bumps");
  t = Translate(P({{"normal", V3(0.0F, 1.0F, 0.0F)}}));
  Diagnosed("a constant normal", t, Kind::UnrepresentedInput, "normal", "Surface");
}

void Diagnostics() {
  // An identifier outside the table, named with its node.
  HdLotusMaterialTranslation t = Colour(
      {kColour, {"If", "ND_ifgreater_color3", {}}}, {{"Image", "out", "If", "in1"}}, "If");
  Diagnosed("an unsupported node", t, Kind::UnsupportedNode, "base_color", "If");
  Check(t.diagnostics.front().node_type == TfToken("ND_ifgreater_color3"),
      "the unsupported node's identifier was not recorded");
  // One input raises one diagnostic: the node nearest the surface.
  t = Colour({kColour, {"Deep", "ND_ifgreater_color3", {}},
                 {"Near", "ND_overlay_color3", {}}, {"Sum", "ND_add_color3", {}},
                 {"Pass", "ND_dot_color3", {}}},
      {{"Deep", "out", "Pass", "in"}, {"Pass", "out", "Sum", "in1"},
          {"Near", "out", "Sum", "in2"}},
      "Sum");
  Diagnosed("the nearest of two stops", t, Kind::UnsupportedNode, "base_color", "Near");
  // A connection to a node that is not there, and a cycle.
  t = Translate({}, {}, {{"Missing", "out", "Surface", "base_color"}});
  Diagnosed("a missing node", t, Kind::UnsupportedGraph, "base_color", "Missing");
  t = Colour({{"A", "ND_dot_color3", {}}, {"B", "ND_dot_color3", {}}},
      {{"A", "out", "B", "in"}, {"B", "out", "A", "in"}}, "A");
  Check(t.diagnostics.size() == 1 && t.diagnostics.front().kind == Kind::UnsupportedGraph,
      "a cycle: " + Describe(t));
  // A type the input does not take.
  t = Translate({}, {kColour}, {{"Image", "out", "Surface", "metalness"}});
  Diagnosed("a colour on a scalar", t, Kind::UnsupportedGraph, "metalness", "Surface");
  t = Translate(P({{"metalness", V3(1.0F, 1.0F, 1.0F)}}));
  Diagnosed("a colour value on a scalar", t, Kind::UnsupportedValue, "metalness", "Surface");
  t = Translate(P({{"specular_roughness", F(std::numeric_limits<float>::quiet_NaN())}}));
  Diagnosed("a non-finite value", t, Kind::UnsupportedValue, "specular_roughness", "Surface");
  Check(Surface(t).roughness == 0.2F, "a non-finite roughness left its default");

  // A lookup where the material holds a constant.
  t = Translate({}, {kScalar}, {{"Scalar", "out", "Surface", "specular_IOR"}});
  Diagnosed("a lookup on the index", t, Kind::UnrepresentedInput, "specular_IOR", "Scalar");
  Check(Surface(t).ior == 1.5F && t.textures.empty(), "a lookup on the index was taken");
  // A weight's lookup spread over its colour, and two lookups in a product.
  t = Translate({}, {kScalar}, {{"Scalar", "out", "Surface", "base"}});
  Diagnosed("a weight lookup over a colour", t, Kind::UnsupportedGraph, "base", "Scalar");
  ExpectValues("a diagnosed weight's default", Surface(t).base_color, {0.8F, 0.8F, 0.8F});

  // The other terminals are reported, and the surface still translates.
  HdMaterialNetworkMap map = Network(P({{"base_color", V3(0.1F, 0.2F, 0.3F)}}),
      {{"Height", "ND_displacement_float", {}}});
  map.map[HdMaterialTerminalTokens->displacement].nodes.push_back(
      map.map[HdMaterialTerminalTokens->surface].nodes.front());
  map.terminals.push_back(SdfPath("/Material/Height"));
  t = HdLotusTranslateMaterial(map);
  Check(t.diagnostics.size() == 1 && t.diagnostics.front().kind == Kind::UnsupportedTerminal &&
            t.diagnostics.front().input.IsEmpty() &&
            t.diagnostics.front().node == SdfPath("/Material/Height") &&
            t.diagnostics.front().node_type == TfToken("ND_displacement_float"),
      "a displacement terminal: " + Describe(t));
  ExpectValues("a surface beside a displacement", Surface(t).base_color, {0.1F, 0.2F, 0.3F});
}

// A hand-built graph.
struct Graph {
  std::vector<Node> nodes;
  std::vector<Link> links;
};

// `graph` with the nodes NG_gltf_image_* place their node Image with: the
// scale inverted, the rotation negated and the offset negated in u.
Graph GltfPlaced(Graph graph, int order) {
  graph.nodes.push_back({"Coords", "ND_texcoord_vector2", {}});
  graph.nodes.push_back({"InvertScale", "ND_divide_vector2",
      P({{"in1", V2(1.0F, 1.0F)}, {"in2", V2(2.0F, 0.5F)}})});
  graph.nodes.push_back({"NegateRotate", "ND_multiply_float",
      P({{"in1", F(30.0F)}, {"in2", F(-1.0F)}})});
  graph.nodes.push_back({"NegateOffset", "ND_multiply_vector2",
      P({{"in1", V2(0.1F, 0.2F)}, {"in2", V2(-1.0F, 1.0F)}})});
  graph.nodes.push_back({"Place", "ND_place2d_vector2",
      P({{"pivot", V2(0.25F, 0.75F)}, {"operationorder", I(order)}})});
  graph.links.push_back({"Coords", "out", "Place", "texcoord"});
  graph.links.push_back({"InvertScale", "out", "Place", "scale"});
  graph.links.push_back({"NegateRotate", "out", "Place", "rotate"});
  graph.links.push_back({"NegateOffset", "out", "Place", "offset"});
  graph.links.push_back({"Place", "out", "Image", "texcoord"});
  return graph;
}

// Each compound node translates as its library graph, transcribed from
// MaterialX 1.39.5's stdlib_ng.mtlx and gltf_pbr.mtlx into primitive nodes.
void CompoundEquivalence() {
  const auto same = [](const std::string& what, const HdLotusMaterialTranslation& compound,
                        const HdLotusMaterialTranslation& primitive) {
    Clean(what + " (compound)", compound);
    Clean(what + " (primitive)", primitive);
    Check(compound.material == primitive.material && compound.textures == primitive.textures,
        what + " differs from its library graph");
    Check(Lotus::HasTextureInputs(compound.material), what + " has no lookup");
  };
  const Node coords{"Coords", "ND_texcoord_vector2", {}};
  const VtValue file = File("colour.png");
  const VtValue srgb(TfToken("srgb_texture"));

  // NG_tiledimage_color3.
  same("ND_tiledimage",
      Colour({{"Image", "ND_tiledimage_color3",
                 P({{"file", file}, {"colorSpace:file", srgb}, {"default", V3(0.1F, 0.2F, 0.3F)},
                     {"uvtiling", V2(2.0F, 3.0F)}, {"uvoffset", V2(0.25F, 0.5F)}})}},
          {}, "Image"),
      Colour({coords, {"Mult", "ND_multiply_vector2", P({{"in2", V2(2.0F, 3.0F)}})},
                 {"Sub", "ND_subtract_vector2", P({{"in2", V2(0.25F, 0.5F)}})},
                 {"DivTile", "ND_divide_vector2", P({{"in2", V2(1.0F, 1.0F)}})},
                 {"MultTile", "ND_multiply_vector2", P({{"in2", V2(1.0F, 1.0F)}})},
                 {"Image", "ND_image_color3",
                     P({{"file", file}, {"colorSpace:file", srgb},
                         {"default", V3(0.1F, 0.2F, 0.3F)}, {"uaddressmode", S("periodic")},
                         {"vaddressmode", S("periodic")}})}},
          {{"Coords", "out", "Mult", "in1"}, {"Mult", "out", "Sub", "in1"},
              {"Sub", "out", "DivTile", "in1"}, {"DivTile", "out", "MultTile", "in1"},
              {"MultTile", "out", "Image", "texcoord"}},
          "Image"));

  // NG_place2d_vector2, one branch of its switch per operation order.
  const Parameters placement = P({{"pivot", V2(0.25F, 0.75F)}, {"scale", V2(2.0F, 0.5F)},
      {"rotate", F(30.0F)}, {"offset", V2(0.1F, 0.2F)}});
  for (const int order : {0, 1}) {
    Parameters place = placement;
    place[TfToken("operationorder")] = I(order);
    const std::vector<Node> first{coords,
        {"SubPivot", "ND_subtract_vector2", P({{"in2", V2(0.25F, 0.75F)}})},
        {"Scale", "ND_divide_vector2", P({{"in2", V2(2.0F, 0.5F)}})},
        {"Rotate", "ND_rotate2d_vector2", P({{"amount", F(30.0F)}})},
        {"Offset", "ND_subtract_vector2", P({{"in2", V2(0.1F, 0.2F)}})},
        {"AddPivot", "ND_add_vector2", P({{"in2", V2(0.25F, 0.75F)}})}, kColour};
    const std::vector<Link> srt{{"Coords", "out", "SubPivot", "in1"},
        {"SubPivot", "out", "Scale", "in1"}, {"Scale", "out", "Rotate", "in"},
        {"Rotate", "out", "Offset", "in1"}, {"Offset", "out", "AddPivot", "in1"},
        {"AddPivot", "out", "Image", "texcoord"}};
    const std::vector<Link> trs{{"Coords", "out", "SubPivot", "in1"},
        {"SubPivot", "out", "Offset", "in1"}, {"Offset", "out", "Rotate", "in"},
        {"Rotate", "out", "Scale", "in1"}, {"Scale", "out", "AddPivot", "in1"},
        {"AddPivot", "out", "Image", "texcoord"}};
    same("ND_place2d in order " + std::to_string(order),
        Colour({coords, {"Place", "ND_place2d_vector2", place}, kColour},
            {{"Coords", "out", "Place", "texcoord"}, {"Place", "out", "Image", "texcoord"}},
            "Image"),
        Colour(first, order == 0 ? srt : trs, "Image"));
  }
  const std::array<float, 6> srt = St(Colour(
      {coords, {"Place", "ND_place2d_vector2", placement}, kColour},
      {{"Coords", "out", "Place", "texcoord"}, {"Place", "out", "Image", "texcoord"}}, "Image"));
  Parameters reordered = placement;
  reordered[TfToken("operationorder")] = I(1);
  Check(srt != St(Colour({coords, {"Place", "ND_place2d_vector2", reordered}, kColour},
                   {{"Coords", "out", "Place", "texcoord"}, {"Place", "out", "Image", "texcoord"}},
                   "Image")),
      "ND_place2d's operation orders do not differ");

  const auto gltf = [&](Parameters parameters) {
    for (const auto& [name, value] : placement) parameters[name] = value;
    parameters[TfToken("file")] = file;
    return parameters;
  };

  // NG_gltf_image_color3_color3_1_0, in both orders.
  for (const int order : {0, 1}) {
    Graph graph = GltfPlaced({
        {{"Image", "ND_image_color3",
             P({{"file", file}, {"colorSpace:file", srgb}, {"default", V3(0.1F, 0.2F, 0.3F)},
                 {"uaddressmode", S("clamp")}, {"vaddressmode", S("mirror")},
                 {"filtertype", S("linear")}})},
            {"ScaleImage", "ND_multiply_color3", P({{"in1", V3(0.5F, 1.0F, 2.0F)}})}},
        {{"Image", "out", "ScaleImage", "in2"}}}, order);
    same("ND_gltf_image_color3 in order " + std::to_string(order),
        Colour({{"Gltf", "ND_gltf_image_color3_color3_1_0",
                   gltf(P({{"colorSpace:file", srgb}, {"default", V3(0.1F, 0.2F, 0.3F)},
                       {"factor", V3(0.5F, 1.0F, 2.0F)}, {"operationorder", I(order)},
                       {"uaddressmode", S("clamp")}, {"vaddressmode", S("mirror")}}))}},
            {}, "Gltf"),
        Colour(graph.nodes, graph.links, "ScaleImage"));
  }
  // NG_gltf_image_float_float_1_0.
  {
    Graph graph = GltfPlaced({
        {{"Image", "ND_image_float", P({{"file", file}, {"default", F(0.25F)}})},
            {"ScaleImage", "ND_multiply_float", P({{"in1", F(0.5F)}})}},
        {{"Image", "out", "ScaleImage", "in2"}}}, 0);
    same("ND_gltf_image_float",
        Scalar({{"Gltf", "ND_gltf_image_float_float_1_0",
                   gltf(P({{"default", F(0.25F)}, {"factor", F(0.5F)}}))}},
            {}, "Gltf"),
        Scalar(graph.nodes, graph.links, "ScaleImage"));
  }
  // The colour-4 image's operation order defaults to 1.
  {
    Graph graph = GltfPlaced({
        {{"Image", "ND_image_color4", P({{"file", file}})},
            {"ScaleImage", "ND_multiply_color4", P({{"in1", V4(0.5F, 1.0F, 2.0F, 1.0F)}})},
            {"Rgb", "ND_convert_color4_color3", {}}},
        {{"Image", "out", "ScaleImage", "in2"}, {"ScaleImage", "out", "Rgb", "in"}}}, 1);
    same("ND_gltf_image_color4",
        Colour({{"Gltf", "ND_gltf_image_color4_color4_1_0",
                    gltf(P({{"factor", V4(0.5F, 1.0F, 2.0F, 1.0F)}}))},
                   {"Rgb", "ND_convert_color4_color3", {}}},
            {{"Gltf", "out", "Rgb", "in"}}, "Rgb"),
        Colour(graph.nodes, graph.links, "Rgb"));
  }

  // NG_gltf_colorimage: the colour-4 glTF image in order 0 with periodic
  // addressing, whatever its own inputs say, times color and geomcolor;
  // outcolor is its RGB and outa its fourth channel.
  for (const bool alpha : {false, true}) {
    Graph graph = GltfPlaced({
        {{"Image", "ND_image_color4", P({{"file", file}, {"colorSpace:file", srgb}})},
            {"ScaleImage", "ND_multiply_color4", {}},
            {"ModulateColor", "ND_multiply_color4", P({{"in1", V4(0.5F, 0.25F, 2.0F, 0.75F)}})},
            {"ModulateGeomColor", "ND_multiply_color4",
                P({{"in2", V4(1.0F, 0.5F, 1.0F, 0.5F)}})},
            {"Separate", "ND_separate4_color4", {}}, {"Combine", "ND_combine3_color3", {}},
            {"Alpha", "ND_dot_float", {}}},
        {{"Image", "out", "ScaleImage", "in2"}, {"ScaleImage", "out", "ModulateColor", "in2"},
            {"ModulateColor", "out", "ModulateGeomColor", "in1"},
            {"ModulateGeomColor", "out", "Separate", "in"},
            {"Separate", "outr", "Combine", "in1"}, {"Separate", "outg", "Combine", "in2"},
            {"Separate", "outb", "Combine", "in3"}, {"Separate", "outa", "Alpha", "in"}}},
        0);
    // The library's scale_image multiplies by the factor's default of 1.
    for (Node& node : graph.nodes) {
      if (node.name == "ScaleImage") node.parameters = P({{"in1", V4(1.0F, 1.0F, 1.0F, 1.0F)}});
    }
    const std::vector<Node> compound{{"Gltf", "ND_gltf_colorimage",
        gltf(P({{"colorSpace:file", srgb}, {"color", V4(0.5F, 0.25F, 2.0F, 0.75F)},
            {"geomcolor", V4(1.0F, 0.5F, 1.0F, 0.5F)}, {"operationorder", I(1)},
            {"uaddressmode", S("clamp")}, {"vaddressmode", S("clamp")}}))}};
    if (alpha) {
      same("ND_gltf_colorimage's outa", Scalar(compound, {}, "Gltf", "outa"),
          Scalar(graph.nodes, graph.links, "Alpha"));
    } else {
      same("ND_gltf_colorimage's outcolor", Colour(compound, {}, "Gltf", "outcolor"),
          Colour(graph.nodes, graph.links, "Combine"));
    }
  }

  // NG_gltf_normalmap_vector3_1_0: the image under ND_normalmap.
  {
    Graph graph = GltfPlaced({
        {{"Image", "ND_image_vector3", P({{"file", file}, {"default", V3(0.5F, 0.5F, 1.0F)}})},
            {"Normal", "ND_normalmap_float", {}}},
        {{"Image", "out", "Normal", "in"}, {"Normal", "out", "Surface", "normal"}}}, 0);
    same("ND_gltf_normalmap",
        Translate({}, {{"Gltf", "ND_gltf_normalmap_vector3_1_0", gltf({})}},
            {{"Gltf", "out", "Surface", "normal"}}),
        Translate({}, graph.nodes, graph.links));
  }
  HdLotusMaterialTranslation t = Colour(
      {{"Gltf", "ND_gltf_image_color3_color3_1_0",
          P({{"file", file}, {"scale", V2(0.0F, 1.0F)}})}},
      {}, "Gltf");
  Diagnosed("a glTF scale of zero", t, Kind::UnsupportedValue, "base_color", "Gltf");
}

// Every input of ND_standard_surface_surfaceshader, authored with a value
// other than its default, is translated, diagnosed, or without effect while
// its lobe's weight is diagnosed.
void Completeness() {
  const HdLotusNodeDefinition* surface = nullptr;
  for (const HdLotusNodeDefinition& definition : HdLotusMaterialNodeDefinitions()) {
    if (definition.identifier == TfToken("ND_standard_surface_surfaceshader")) {
      surface = &definition;
    }
  }
  Check(surface != nullptr && surface->inputs.size() == 42,
      "the translator holds no Standard Surface definition of 42 inputs");
  // What each input does when authored alone.
  const std::set<std::string> translated{"base", "base_color", "metalness",
      "specular_roughness", "specular_IOR", "transmission", "emission", "opacity",
      "thin_walled"};
  const std::set<std::string> unrepresented{"diffuse_roughness", "specular",
      "specular_color", "specular_anisotropy", "subsurface", "sheen", "coat",
      "thin_film_thickness", "normal"};
  // Inputs of a lobe, the medium or a workflow that is off by default.
  const std::map<std::string, std::string> governed{{"specular_rotation", "specular_anisotropy"},
      {"transmission_color", "transmission"}, {"transmission_depth", "transmission"},
      {"transmission_scatter", "transmission"},
      {"transmission_scatter_anisotropy", "transmission"},
      {"transmission_dispersion", "transmission"},
      {"transmission_extra_roughness", "transmission"}, {"subsurface_color", "subsurface"},
      {"subsurface_radius", "subsurface"}, {"subsurface_scale", "subsurface"},
      {"subsurface_anisotropy", "subsurface"}, {"sheen_color", "sheen"},
      {"sheen_roughness", "sheen"}, {"coat_color", "coat"}, {"coat_roughness", "coat"},
      {"coat_anisotropy", "coat"}, {"coat_rotation", "coat"}, {"coat_IOR", "coat"},
      {"coat_normal", "coat"}, {"coat_affect_color", "coat"},
      {"coat_affect_roughness", "coat"}, {"thin_film_IOR", "thin_film_thickness"},
      {"emission_color", "emission"}, {"tangent", "specular_anisotropy"}};
  const auto authored = [](const HdLotusNodeInputDefinition& input) {
    if (input.type == TfToken("boolean")) return VtValue(true);
    if (input.type == TfToken("float")) return F(0.375F);
    // Grey: a coloured opacity would be substituted by its mean.
    return V3(0.375F, 0.375F, 0.375F);
  };
  const HdLotusMaterialTranslation defaults = Translate({});
  Clean("the defaults", defaults);
  Parameters everything;
  for (const HdLotusNodeInputDefinition& input : surface->inputs) {
    const std::string name = input.name.GetString();
    everything[input.name] = authored(input);
    const HdLotusMaterialTranslation alone = Translate({{input.name, authored(input)}});
    const bool changed = !(alone.material == defaults.material);
    if (translated.contains(name)) {
      // emission scales a colour that is white by default.
      Check(changed && alone.diagnostics.empty(),
          name + " is not translated: " + Describe(alone));
    } else if (unrepresented.contains(name)) {
      Check(alone.diagnostics.size() == 1 && alone.diagnostics.front().input == input.name &&
                alone.diagnostics.front().kind == Kind::UnrepresentedInput,
          name + " is not reported as unrepresented: " + Describe(alone));
    } else {
      const auto governor = governed.find(name);
      Check(governor != governed.end(), name + " is in none of the test's tables");
      Check(!changed && alone.diagnostics.empty(),
          name + " has an effect without " + governor->second + ": " + Describe(alone));
      // With its governor on, either the input shows or the governor is
      // diagnosed.
      const HdLotusNodeInputDefinition* weight = nullptr;
      for (const HdLotusNodeInputDefinition& candidate : surface->inputs) {
        if (candidate.name.GetString() == governor->second) weight = &candidate;
      }
      Check(weight != nullptr, name + "'s governor is not an input");
      const HdLotusMaterialTranslation off = Translate({{weight->name, authored(*weight)}});
      const HdLotusMaterialTranslation on = Translate(
          {{weight->name, authored(*weight)}, {input.name, authored(input)}});
      const bool governor_diagnosed =
          std::any_of(on.diagnostics.begin(), on.diagnostics.end(),
              [&](const HdLotusMaterialDiagnostic& diagnostic) {
                return diagnostic.input == weight->name || diagnostic.input == input.name;
              });
      Check(governor_diagnosed || !(on.material == off.material),
          name + " is neither translated nor diagnosed under " + governor->second);
    }
  }
  Check(translated.size() + unrepresented.size() + governed.size() == surface->inputs.size(),
      "the test's tables do not cover the definition");
  // Everything at once: each diagnostic names an input of the definition,
  // once.
  const HdLotusMaterialTranslation all = Translate(everything);
  std::set<TfToken> seen;
  for (const HdLotusMaterialDiagnostic& diagnostic : all.diagnostics) {
    Check(seen.insert(diagnostic.input).second,
        diagnostic.input.GetString() + " is diagnosed twice: " + Describe(all));
  }
  // An input the definition does not name.
  HdLotusMaterialTranslation t = Translate(P({{"base_colour", V3(1.0F, 0.0F, 0.0F)}}));
  Diagnosed("an unknown parameter", t, Kind::UnknownInput, "base_colour", "Surface");
  t = Translate({}, {kColour}, {{"Image", "out", "Surface", "diffuse_color"}});
  Diagnosed("an unknown connection", t, Kind::UnknownInput, "diffuse_color", "Surface");
}

// MaterialX's glTF PBR surface (docs/design/GLTF_PBR.md section 5): its
// input table, alpha modes, the networks usd-gltf-plugins authors, and every
// input of the definition as translated, diagnosed or without effect.
void GltfPbr() {
  const auto gltf = [](const HdLotusMaterialTranslation& translation)
      -> const Lotus::GltfPbr& {
    Check(translation.material.Model() == Lotus::ShadingModel::GltfPbr,
        "a glTF PBR surface translates to another model");
    return Lotus::Gltf(translation.material);
  };
  // The definition's defaults: a rough white metal, opaque.
  const HdLotusMaterialTranslation defaults = TranslateGltf({});
  Clean("glTF PBR's defaults", defaults);
  Lotus::Material expected;
  expected.surface.emplace<Lotus::GltfPbr>();
  Check(defaults.material == expected, "glTF PBR's defaults are not the definition's");
  Check(gltf(defaults).base_color.value == std::array<float, 3>{1.0F, 1.0F, 1.0F} &&
            gltf(defaults).metallic.value == 1.0F && gltf(defaults).roughness.value == 1.0F,
      "the IR's glTF PBR defaults are not a rough white metal");

  // Constants, clamped into their ranges.
  HdLotusMaterialTranslation t = TranslateGltf(P({{"base_color", V3(0.2F, 0.4F, 0.6F)},
      {"metallic", F(0.25F)}, {"roughness", F(0.75F)}, {"emissive", V3(2.0F, 1.0F, 0.5F)},
      {"emissive_strength", F(3.0F)}}));
  Clean("glTF PBR constants", t);
  ExpectValues("base_color", gltf(t).base_color.value, {0.2F, 0.4F, 0.6F});
  ExpectValues("emissive", gltf(t).emissive.value, {2.0F, 1.0F, 0.5F});
  Check(gltf(t).metallic.value == 0.25F && gltf(t).roughness.value == 0.75F &&
            gltf(t).emissive_strength == 3.0F,
      "glTF PBR's scalar constants are not as authored");
  t = TranslateGltf(P({{"base_color", V3(1.25F, 0.5F, -0.25F)}, {"metallic", F(-1.0F)},
      {"roughness", F(1.5F)}, {"emissive", V3(-1.0F, 0.5F, 4.0F)},
      {"emissive_strength", F(-2.0F)}}));
  Clean("glTF PBR clamped constants", t);
  ExpectValues("clamped base_color", gltf(t).base_color.value, {1.0F, 0.5F, 0.0F});
  ExpectValues("clamped emissive", gltf(t).emissive.value, {0.0F, 0.5F, 4.0F});
  Check(gltf(t).metallic.value == 0.0F && gltf(t).roughness.value == 1.0F &&
            gltf(t).emissive_strength == 0.0F,
      "glTF PBR's scalar constants are not clamped");
  t = TranslateGltf({}, {kScalar}, {{"Scalar", "out", "Surface", "emissive_strength"}});
  Diagnosed("a connected emissive_strength", t, Kind::UnrepresentedInput,
      "emissive_strength", "Scalar");

  // Alpha: OPAQUE reads none, MASK compares it with the cutoff, BLEND is
  // stochastic coverage.
  const auto coverage = [&](const char* what, Parameters surface, float opacity,
                            float threshold) {
    const HdLotusMaterialTranslation alpha = TranslateGltf(std::move(surface));
    Clean(what, alpha);
    Check(alpha.material.geometry.opacity == opacity &&
              alpha.material.geometry.opacity_threshold == threshold &&
              !alpha.material.geometry.opacity_texture,
        std::string(what) + ": opacity " +
            std::to_string(alpha.material.geometry.opacity) + " threshold " +
            std::to_string(alpha.material.geometry.opacity_threshold));
  };
  coverage("OPAQUE", P({{"alpha", F(0.3F)}}), 1.0F, 0.0F);
  coverage("OPAQUE with a cutoff", P({{"alpha", F(0.3F)}, {"alpha_cutoff", F(0.9F)}}),
      1.0F, 0.0F);
  coverage("MASK", P({{"alpha", F(0.7F)}, {"alpha_mode", I(1)}}), 0.7F, 0.5F);
  coverage("MASK with a cutoff",
      P({{"alpha", F(0.7F)}, {"alpha_mode", I(1)}, {"alpha_cutoff", F(0.25F)}}), 0.7F, 0.25F);
  coverage("MASK at a cutoff of 0",
      P({{"alpha", F(0.2F)}, {"alpha_mode", I(1)}, {"alpha_cutoff", F(0.0F)}}), 1.0F, 0.0F);
  coverage("MASK at a cutoff above 1",
      P({{"alpha", F(1.0F)}, {"alpha_mode", I(1)}, {"alpha_cutoff", F(1.5F)}}), 0.0F, 0.0F);
  coverage("BLEND", P({{"alpha", F(0.4F)}, {"alpha_mode", I(2)}}), 0.4F, 0.0F);
  coverage("BLEND clamped", P({{"alpha", F(1.5F)}, {"alpha_mode", I(2)}}), 1.0F, 0.0F);
  t = TranslateGltf(P({{"alpha", F(0.4F)}, {"alpha_mode", I(3)}}));
  Diagnosed("an alpha_mode of 3", t, Kind::UnsupportedValue, "alpha_mode", "Surface");
  Check(t.material.geometry.opacity == 1.0F, "an unknown alpha_mode is not opaque");
  t = TranslateGltf(P({{"alpha", F(0.4F)}}), {kScalar},
      {{"Scalar", "out", "Surface", "alpha_mode"}});
  Diagnosed("a connected alpha_mode", t, Kind::UnsupportedValue, "alpha_mode", "Surface");
  Check(t.material.geometry.opacity == 1.0F, "a connected alpha_mode is not opaque");
  t = TranslateGltf(P({{"alpha", F(0.4F)}, {"alpha_mode", I(1)}}), {kScalar},
      {{"Scalar", "out", "Surface", "alpha_cutoff"}});
  Diagnosed("a connected alpha_cutoff", t, Kind::UnsupportedValue, "alpha_cutoff",
      "Surface");
  Check(t.material.geometry.opacity == 1.0F, "a connected alpha_cutoff is not opaque");

  // The normal input: a normal map, never an authored world-space constant.
  t = TranslateGltf(P({{"normal", V3(0.0F, 1.0F, 0.0F)}}));
  Diagnosed("a constant normal", t, Kind::UnrepresentedInput, "normal", "Surface");
  t = TranslateGltf({},
      {{"Normal", "ND_gltf_normalmap_vector3_1_0", P({{"file", File("bumps.png")}})}},
      {{"Normal", "out", "Surface", "normal"}});
  Clean("ND_gltf_normalmap on glTF PBR", t);
  Check(t.material.geometry.normal_texture.has_value(),
      "ND_gltf_normalmap on glTF PBR is no lookup");

  // The extensions (docs/design/GLTF_PBR_EXTENSIONS.md section 5): constants
  // clamped into their ranges.
  t = TranslateGltf(P({{"ior", F(1.8F)}, {"specular", F(0.5F)},
      {"specular_color", V3(2.0F, 0.5F, 0.25F)}, {"transmission", F(0.75F)},
      {"clearcoat", F(0.6F)}, {"clearcoat_roughness", F(0.2F)},
      {"sheen_color", V3(0.2F, 0.4F, 0.8F)}, {"sheen_roughness", F(0.3F)}}));
  Clean("the extensions' constants", t);
  ExpectValues("specular_color", gltf(t).specular_color.value, {2.0F, 0.5F, 0.25F});
  ExpectValues("sheen_color", gltf(t).sheen_color.value, {0.2F, 0.4F, 0.8F});
  Check(gltf(t).ior == 1.8F && gltf(t).specular.value == 0.5F &&
            gltf(t).transmission.value == 0.75F && gltf(t).clearcoat.value == 0.6F &&
            gltf(t).clearcoat_roughness.value == 0.2F &&
            gltf(t).sheen_roughness.value == 0.3F,
      "the extensions' constants are not the authored ones");
  t = TranslateGltf(P({{"ior", F(0.5F)}, {"specular", F(1.5F)},
      {"specular_color", V3(-1.0F, 0.5F, 3.0F)}, {"transmission", F(-0.5F)},
      {"clearcoat", F(2.0F)}, {"clearcoat_roughness", F(-1.0F)},
      {"sheen_color", V3(1.5F, 0.5F, -0.5F)}, {"sheen_roughness", F(4.0F)}}));
  Clean("the extensions' constants out of range", t);
  ExpectValues("clamped specular_color", gltf(t).specular_color.value, {0.0F, 0.5F, 3.0F});
  ExpectValues("clamped sheen_color", gltf(t).sheen_color.value, {1.0F, 0.5F, 0.0F});
  Check(gltf(t).ior == 1.0F && gltf(t).specular.value == 1.0F &&
            gltf(t).transmission.value == 0.0F && gltf(t).clearcoat.value == 1.0F &&
            gltf(t).clearcoat_roughness.value == 0.0F &&
            gltf(t).sheen_roughness.value == 1.0F,
      "the extensions' constants are not clamped");
  // The index is a uniform of the definition: a lookup is not read.
  t = TranslateGltf({}, {kScalar}, {{"Scalar", "out", "Surface", "ior"}});
  Diagnosed("a connected ior", t, Kind::UnrepresentedInput, "ior", "Scalar");
  Check(gltf(t).ior == 1.5F, "a connected ior does not keep the default index");
  // A lookup on each textured input.
  for (const char* input : {"specular", "transmission", "clearcoat"}) {
    t = TranslateGltf({}, {kScalar}, {{"Scalar", "out", "Surface", input}});
    Clean(std::string("a lookup on ") + input, t);
    const Lotus::GltfPbr& read = gltf(t);
    const std::optional<Lotus::TextureInput>& lookup =
        std::string(input) == "specular" ? read.specular.texture
        : std::string(input) == "transmission" ? read.transmission.texture
                                                 : read.clearcoat.texture;
    Check(lookup.has_value(), std::string(input) + " takes no lookup");
  }
  for (const char* input : {"specular_color", "sheen_color"}) {
    t = TranslateGltf({}, {kColour}, {{"Image", "out", "Surface", input}});
    Clean(std::string("a lookup on ") + input, t);
    Check((std::string(input) == "specular_color" ? gltf(t).specular_color.texture
                                                   : gltf(t).sheen_color.texture)
              .has_value(),
        std::string(input) + " takes no lookup");
  }
  // A lobe's other inputs are read with its weight: under a textured
  // clearcoat, its roughness and its normal map; under a sheen, its
  // roughness.
  t = TranslateGltf(P({{"sheen_color", V3(0.5F, 0.5F, 0.5F)}}),
      {kScalar, kPacked,
          {"CoatNormal", "ND_normalmap_float", {}}},
      {{"Scalar", "out", "Surface", "clearcoat"},
          {"Scalar", "out", "Surface", "clearcoat_roughness"},
          {"Scalar", "out", "Surface", "sheen_roughness"},
          {"Packed", "out", "CoatNormal", "in"},
          {"CoatNormal", "out", "Surface", "clearcoat_normal"}});
  Clean("a clearcoat with a roughness lookup and a normal map", t);
  Check(gltf(t).clearcoat_roughness.texture.has_value() &&
            gltf(t).clearcoat_normal_texture.has_value() &&
            gltf(t).sheen_roughness.texture.has_value() &&
            !t.material.geometry.normal_texture.has_value(),
      "the clearcoat's and the sheen's inputs are not lookups");
  // Without the weight they are not read at all.
  t = TranslateGltf({}, {kScalar, kPacked, {"CoatNormal", "ND_normalmap_float", {}}},
      {{"Scalar", "out", "Surface", "clearcoat_roughness"},
          {"Scalar", "out", "Surface", "sheen_roughness"},
          {"Packed", "out", "CoatNormal", "in"},
          {"CoatNormal", "out", "Surface", "clearcoat_normal"}});
  Clean("a clearcoat's inputs without a clearcoat", t);
  Check(t.material == defaults.material, "a lobe's inputs are read without its weight");
  // The coat's normal is a normal map, never an authored constant, and a
  // normal map reaches no other input.
  t = TranslateGltf(P({{"clearcoat", F(1.0F)}, {"clearcoat_normal", V3(0.0F, 1.0F, 0.0F)}}));
  Diagnosed("a constant clearcoat normal", t, Kind::UnrepresentedInput, "clearcoat_normal",
      "Surface");
  // Transmission is a thin surface whatever the thickness, which is
  // reported.
  t = TranslateGltf(P({{"transmission", F(1.0F)}, {"thickness", F(0.5F)}}));
  Diagnosed("a thickness beside a transmission", t, Kind::UnrepresentedInput, "thickness",
      "Surface");
  Check(gltf(t).transmission.value == 1.0F, "a thickness removes the transmission");
  // usd-gltf-plugins' KHR_materials_unlit: a black base that only emits,
  // without a reflection lobe.
  t = TranslateGltf(P({{"base_color", V3(0.0F, 0.0F, 0.0F)}, {"specular", F(0.0F)},
      {"metallic", F(0.0F)}, {"emissive", V3(1.0F, 0.5F, 0.25F)}}));
  Clean("the plugin's unlit material", t);
  Check(gltf(t).specular.value == 0.0F, "the unlit material keeps a reflection lobe");

  // The networks usd-gltf-plugins authors: a colour-4 base colour image
  // whose alpha is the coverage, times a factor; a vector-4 image packing
  // roughness in green and metallic in blue; a vector-4 normal map; an sRGB
  // emissive image; all at coordinates under KHR_texture_transform.
  const std::vector<Node> plugin{
      {"Coords", "ND_geompropvalue_vector2", P({{"geomprop", S("st")}})},
      {"Scaled", "ND_multiply_vector2", P({{"in2", V2(2.0F, 3.0F)}})},
      {"Turned", "ND_rotate2d_vector2", P({{"amount", F(30.0F)}})},
      {"Moved", "ND_add_vector2", P({{"in2", V2(0.25F, 0.5F)}})},
      {"Base", "ND_image_color4",
          {{TfToken("file"), File("base.png")},
              {TfToken("colorSpace:file"), VtValue(TfToken("srgb_texture"))}}},
      {"BaseRgb", "ND_convert_color4_color3", {}},
      {"BaseTint", "ND_multiply_color3", P({{"in2", V3(1.0F, 0.5F, 0.25F)}})},
      {"BaseAlpha", "ND_extract_color4", P({{"index", I(3)}})},
      {"AlphaFactor", "ND_multiply_float", P({{"in2", F(0.5F)}})},
      {"Orm", "ND_image_vector4", P({{"file", File("orm.png")}})},
      {"Roughness", "ND_extract_vector4", P({{"index", I(1)}})},
      {"Metallic", "ND_extract_vector4", P({{"index", I(2)}})},
      {"Bumps", "ND_image_vector4", P({{"file", File("bumps.png")}})},
      {"BumpsXyz", "ND_convert_vector4_vector3", {}},
      {"Normal", "ND_normalmap_float", {}},
      {"Glow", "ND_image_color4",
          {{TfToken("file"), File("glow.png")},
              {TfToken("colorSpace:file"), VtValue(TfToken("srgb_texture"))}}},
      {"GlowRgb", "ND_convert_color4_color3", {}}};
  std::vector<Link> wires{{"Coords", "out", "Scaled", "in1"},
      {"Scaled", "out", "Turned", "in"}, {"Turned", "out", "Moved", "in1"},
      {"Base", "out", "BaseRgb", "in"}, {"BaseRgb", "out", "BaseTint", "in1"},
      {"BaseTint", "out", "Surface", "base_color"}, {"Base", "out", "BaseAlpha", "in"},
      {"BaseAlpha", "out", "AlphaFactor", "in1"}, {"AlphaFactor", "out", "Surface", "alpha"},
      {"Orm", "out", "Roughness", "in"}, {"Roughness", "out", "Surface", "roughness"},
      {"Orm", "out", "Metallic", "in"}, {"Metallic", "out", "Surface", "metallic"},
      {"Bumps", "out", "BumpsXyz", "in"}, {"BumpsXyz", "out", "Normal", "in"},
      {"Normal", "out", "Surface", "normal"}, {"Glow", "out", "GlowRgb", "in"},
      {"GlowRgb", "out", "Surface", "emissive"}};
  for (const char* image : {"Base", "Orm", "Bumps", "Glow"}) {
    wires.push_back({"Moved", "out", image, "texcoord"});
  }
  t = TranslateGltf(P({{"alpha_mode", I(2)}, {"emissive_strength", F(2.0F)}}), plugin, wires);
  Clean("the plugin's glTF network", t);
  const Lotus::GltfPbr& authored = gltf(t);
  Check(authored.base_color.texture && authored.metallic.texture &&
            authored.roughness.texture && authored.emissive.texture &&
            t.material.geometry.normal_texture && t.material.geometry.opacity_texture,
      "the plugin's glTF network leaves an input without its lookup");
  Check(t.material.geometry.texcoords == "st", "the plugin's glTF network reads another set");
  const Lotus::TextureInput& base = *authored.base_color.texture;
  Check(base.texture == HdLotusTextureKey("base.png", TfToken("sRGB")) &&
            base.scale[0] == 1.0F && base.scale[1] == 0.5F && base.scale[2] == 0.25F &&
            base.st_transform != Lotus::TextureInput{}.st_transform,
      "the plugin's base colour is " + Describe(base));
  Check(t.material.geometry.opacity_texture->channel == 3 &&
            t.material.geometry.opacity_texture->scale[3] == 0.5F &&
            t.material.geometry.opacity_texture->st_transform == base.st_transform,
      "the plugin's alpha is " + Describe(*t.material.geometry.opacity_texture));
  Check(authored.roughness.texture->channel == 1 && authored.metallic.texture->channel == 2 &&
            authored.roughness.texture->texture ==
                HdLotusTextureKey("orm.png", TfToken("raw")),
      "the plugin's packed image is " + Describe(*authored.roughness.texture));
  Check(authored.emissive.texture->texture == HdLotusTextureKey("glow.png", TfToken("sRGB")) &&
            authored.emissive_strength == 2.0F,
      "the plugin's emissive is " + Describe(*authored.emissive.texture));
  // Vertex colour is not translated: the node is reported, and the input
  // keeps its default.
  t = TranslateGltf({},
      {{"Color", "ND_geompropvalue_color3", P({{"geomprop", S("displayColor")}})},
          {"Tint", "ND_multiply_color3", P({{"in2", V3(1.0F, 0.5F, 0.25F)}})}},
      {{"Color", "out", "Tint", "in1"}, {"Tint", "out", "Surface", "base_color"}});
  Diagnosed("a vertex colour", t, Kind::UnsupportedNode, "base_color", "Color");
  ExpectValues("base_color under a vertex colour", gltf(t).base_color.value,
      {1.0F, 1.0F, 1.0F});
  // Occlusion does nothing in a path tracer: its graph is not read, even at
  // a second coordinate set.
  t = TranslateGltf({},
      {kColour, {"Uv1", "ND_geompropvalue_vector2", P({{"geomprop", S("uv1")}})},
          {"Ao", "ND_image_float", P({{"file", File("ao.png")}})}},
      {{"Image", "out", "Surface", "base_color"}, {"Uv1", "out", "Ao", "texcoord"},
          {"Ao", "out", "Surface", "occlusion"}});
  Clean("occlusion at a second coordinate set", t);

  // Every input of the definition, authored alone with a value other than
  // its default.
  const HdLotusNodeDefinition* surface = nullptr;
  for (const HdLotusNodeDefinition& definition : HdLotusMaterialNodeDefinitions()) {
    if (definition.identifier == TfToken("ND_gltf_pbr_surfaceshader")) surface = &definition;
  }
  Check(surface != nullptr && surface->inputs.size() == 29,
      "the translator holds no glTF PBR definition of 29 inputs");
  // alpha_mode alone is MASK at the default cutoff, over an alpha of 1.
  const std::set<std::string> translated{"base_color", "metallic", "roughness", "emissive",
      "emissive_strength", "alpha_mode", "transmission", "specular", "specular_color", "ior",
      "sheen_color", "clearcoat"};
  const std::set<std::string> unrepresented{"iridescence", "anisotropy_strength", "thickness",
      "attenuation_distance", "attenuation_color", "dispersion", "normal"};
  // Inputs that do nothing alone, and what turns them on; occlusion never
  // does anything.
  const std::map<std::string, std::string> governed{{"occlusion", ""},
      {"tangent", "anisotropy_strength"}, {"alpha", "alpha_mode"},
      {"alpha_cutoff", "alpha_mode"},
      {"iridescence_ior", "iridescence"}, {"iridescence_thickness", "iridescence"},
      {"sheen_roughness", "sheen_color"}, {"clearcoat_roughness", "clearcoat"},
      {"clearcoat_normal", "clearcoat"}, {"anisotropy_rotation", "anisotropy_strength"}};
  const auto value = [](const HdLotusNodeInputDefinition& input) {
    // MASK, under which alpha and its cutoff both show.
    if (input.type == TfToken("integer")) return I(1);
    if (input.type == TfToken("float")) return F(0.375F);
    return V3(0.375F, 0.375F, 0.375F);
  };
  Parameters everything;
  for (const HdLotusNodeInputDefinition& input : surface->inputs) {
    const std::string name = input.name.GetString();
    everything[input.name] = value(input);
    const HdLotusMaterialTranslation alone = TranslateGltf({{input.name, value(input)}});
    const bool changed = !(alone.material == defaults.material);
    if (translated.contains(name)) {
      Check(changed && alone.diagnostics.empty(),
          name + " is not translated: " + Describe(alone));
    } else if (unrepresented.contains(name)) {
      Check(!changed && alone.diagnostics.size() == 1 &&
                alone.diagnostics.front().input == input.name &&
                alone.diagnostics.front().kind == Kind::UnrepresentedInput,
          name + " is not reported as unrepresented: " + Describe(alone));
    } else {
      const auto governor = governed.find(name);
      Check(governor != governed.end(), name + " is in none of the test's tables");
      Check(!changed && alone.diagnostics.empty(),
          name + " has an effect alone: " + Describe(alone));
      if (governor->second.empty()) continue;
      const HdLotusNodeInputDefinition* weight = nullptr;
      for (const HdLotusNodeInputDefinition& candidate : surface->inputs) {
        if (candidate.name.GetString() == governor->second) weight = &candidate;
      }
      Check(weight != nullptr, name + "'s governor is not an input");
      const HdLotusMaterialTranslation off = TranslateGltf({{weight->name, value(*weight)}});
      const HdLotusMaterialTranslation on = TranslateGltf(
          {{weight->name, value(*weight)}, {input.name, value(input)}});
      const bool diagnosed = std::any_of(on.diagnostics.begin(), on.diagnostics.end(),
          [&](const HdLotusMaterialDiagnostic& diagnostic) {
            return diagnostic.input == weight->name || diagnostic.input == input.name;
          });
      Check(diagnosed || !(on.material == off.material),
          name + " is neither translated nor diagnosed under " + governor->second);
    }
  }
  Check(translated.size() + unrepresented.size() + governed.size() == surface->inputs.size(),
      "the test's glTF PBR tables do not cover the definition");
  const HdLotusMaterialTranslation all = TranslateGltf(everything);
  std::set<TfToken> seen;
  for (const HdLotusMaterialDiagnostic& diagnostic : all.diagnostics) {
    Check(seen.insert(diagnostic.input).second,
        diagnostic.input.GetString() + " is diagnosed twice: " + Describe(all));
  }
  // Beside the unrepresented inputs, the coat's constant normal, which is
  // read under a clearcoat.
  Check(seen.size() == unrepresented.size() + 1 && seen.contains(TfToken("clearcoat_normal")),
      "everything at once raises " + std::to_string(seen.size()) + " diagnostics: " +
          Describe(all));
  t = TranslateGltf(P({{"baseColor", V3(1.0F, 0.0F, 0.0F)}}));
  Diagnosed("an unknown glTF PBR parameter", t, Kind::UnknownInput, "baseColor", "Surface");
}

// A value's components, whatever numeric type holds it.
std::optional<std::vector<double>> Components(const VtValue& value) {
  if (value.IsHolding<float>()) return std::vector<double>{value.UncheckedGet<float>()};
  if (value.IsHolding<double>()) return std::vector<double>{value.UncheckedGet<double>()};
  if (value.IsHolding<int>()) return std::vector<double>{double(value.UncheckedGet<int>())};
  if (value.IsHolding<bool>()) return std::vector<double>{value.UncheckedGet<bool>() ? 1.0 : 0.0};
  if (value.IsHolding<GfVec2f>()) {
    const GfVec2f v = value.UncheckedGet<GfVec2f>();
    return std::vector<double>{v[0], v[1]};
  }
  if (value.IsHolding<GfVec3f>()) {
    const GfVec3f v = value.UncheckedGet<GfVec3f>();
    return std::vector<double>{v[0], v[1], v[2]};
  }
  if (value.IsHolding<GfVec4f>()) {
    const GfVec4f v = value.UncheckedGet<GfVec4f>();
    return std::vector<double>{v[0], v[1], v[2], v[3]};
  }
  return std::nullopt;
}

std::string Text(const VtValue& value) {
  // Sdr gives an empty string's default no value.
  if (value.IsEmpty()) return {};
  if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
  if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
  if (value.IsHolding<SdfAssetPath>()) return value.UncheckedGet<SdfAssetPath>().GetAssetPath();
  return "<" + value.GetTypeName() + ">";
}

// Every definition the translator holds is the runtime's: the same inputs,
// of the same types, with the same defaults; and the surfaces' definitions
// are the pinned versions (docs/design/MATERIAL_IR_V2.md section 3).
void Definitions(bool dump) {
  SdrRegistry& registry = SdrRegistry::GetInstance();
  std::size_t inputs = 0;
  for (const HdLotusNodeDefinition& definition : HdLotusMaterialNodeDefinitions()) {
    const std::string id = definition.identifier.GetString();
    const SdrShaderNodeConstPtr node = registry.GetShaderNodeByIdentifier(definition.identifier);
    Check(node != nullptr, "the runtime has no node definition " + id);
    std::set<std::string> held, runtime;
    for (const HdLotusNodeInputDefinition& input : definition.inputs) {
      held.insert(input.name.GetString());
    }
    for (const TfToken& name : node->GetShaderInputNames()) runtime.insert(name.GetString());
    std::string difference;
    for (const std::string& name : held) {
      if (!runtime.contains(name)) difference += " -" + name;
    }
    for (const std::string& name : runtime) {
      if (!held.contains(name)) difference += " +" + name;
    }
    Check(difference.empty(), id + "'s inputs differ from the runtime's:" + difference);
    for (const HdLotusNodeInputDefinition& input : definition.inputs) {
      const SdrShaderPropertyConstPtr property = node->GetShaderInput(input.name);
      const std::string what = id + "." + input.name.GetString();
      const VtValue value = property->GetDefaultValueAsSdfType();
      const std::string sdf = property->GetTypeAsSdfType().GetSdfType().GetAsToken().GetString();
      if (dump) {
        std::cout << what << " : " << input.type << " = sdf " << sdf << " " << value << '\n';
      }
      static const std::map<std::string, std::set<std::string>> kSdfTypes{
          {"float", {"float"}}, {"color3", {"color3f"}}, {"color4", {"color4f"}},
          {"vector2", {"float2"}}, {"vector3", {"float3", "vector3f"}},
          {"vector4", {"float4"}}, {"integer", {"int"}}, {"boolean", {"bool"}},
          {"string", {"string", "token"}}, {"filename", {"asset"}}};
      Check(kSdfTypes.at(input.type.GetString()).contains(sdf),
          what + " is a " + input.type.GetString() + ", the runtime's a " + sdf);
      if (!input.has_value) {
        continue;
      }
      if (input.type == TfToken("string") || input.type == TfToken("filename")) {
        Check(Text(value) == input.text,
            what + " defaults to '" + input.text + "', the runtime's to '" + Text(value) + "'");
      } else {
        const auto components = Components(value);
        Check(components.has_value() && components->size() == input.value.size(),
            what + "'s default has another shape than the runtime's " + value.GetTypeName());
        for (std::size_t c = 0; c < input.value.size(); ++c) {
          Check(Fold(input.value[c]) == Fold((*components)[c]),
              what + "'s default component " + std::to_string(c) + " is " +
                  std::to_string(input.value[c]) + ", the runtime's " +
                  std::to_string((*components)[c]));
        }
      }
      ++inputs;
    }
  }
  // The surfaces' versions. Sdr does not carry a MaterialX definition's
  // version, so it is read from the document Sdr resolved the node to.
  for (const auto& [id, version] :
      {std::pair{"ND_gltf_pbr_surfaceshader", "2.0.1"},
          std::pair{"ND_standard_surface_surfaceshader", "1.0.1"},
          std::pair{"ND_open_pbr_surface_surfaceshader", "1.1.1"}}) {
    const SdrShaderNodeConstPtr node = registry.GetShaderNodeByIdentifier(TfToken(id));
    Check(node != nullptr, std::string("the runtime has no surface ") + id);
    const std::string document = node->GetResolvedDefinitionURI();
    std::ifstream stream(document);
    const std::string text((std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    const std::size_t tag = text.find(std::string("<nodedef name=\"") + id + "\"");
    Check(tag != std::string::npos, document + " does not define " + id);
    const std::string element = text.substr(tag, text.find('>', tag) - tag);
    const std::string attribute = " version=\"";
    const std::size_t at = element.find(attribute);
    const std::string runtime =
        at == std::string::npos
            ? std::string("unversioned")
            : element.substr(at + attribute.size(),
                  element.find('"', at + attribute.size()) - at - attribute.size());
    Check(runtime == version, std::string(id) + " is version " + runtime +
                                  " in the runtime, and " + version + " is pinned");
  }
  std::cout << HdLotusMaterialNodeDefinitions().size() << " node definitions and " << inputs
            << " defaults are the runtime's\n";
}

class SyncTask final : public HdTask {
public:
  SyncTask(HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state)
      : HdTask(SdfPath::EmptyPath()), pass_(std::move(pass)), state_(std::move(state)) {
  }
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirty_bits) override {
    pass_->Sync();
    *dirty_bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {
  }
  void Execute(HdTaskContext*) override {
    pass_->Execute(state_, GetRenderTags());
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }

private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

// The materials a committed stage extracts to, by path.
std::map<std::string, Lotus::Material> StageMaterials(
    const std::filesystem::path& name) {
  const std::filesystem::path file = name.is_absolute() ? name : kFixtures / name;
  const UsdStageRefPtr stage = UsdStage::Open(file.string());
  Check(stage != nullptr, "could not open " + file.string());
  HdLotusRenderDelegate delegate;
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(index != nullptr, "could not create the render index");
  UsdImagingCreateSceneIndicesInfo info;
  info.stage = stage;
  const UsdImagingSceneIndices scene_indices = UsdImagingCreateSceneIndices(info);
  scene_indices.stageSceneIndex->SetTime(UsdTimeCode::Default());
  index->InsertSceneIndex(scene_indices.finalSceneIndex, SdfPath::AbsoluteRootPath());
  HdTaskSharedPtrVector tasks{std::make_shared<SyncTask>(
      delegate.CreateRenderPass(index.get(),
          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->smoothHull))),
      std::make_shared<HdRenderPassState>())};
  HdEngine engine;
  scene_indices.stageSceneIndex->ApplyPendingUpdates();
  engine.Execute(index.get(), &tasks);
  const Lotus::FrameSnapshot snapshot = delegate.GetFrameSnapshot();
  return {snapshot.scene->materials.begin(), snapshot.scene->materials.end()};
}

// A committed stage's UsdShade network and the .mtlx document it references
// reach the translator as the same network (contract section 3).
void StageRoundTrip() {
  const auto materials = StageMaterials("graph-stage-v1.usda");
  std::string keys;
  for (const auto& [key, material] : materials) keys += " " + key;
  const auto authored = materials.find("/GraphStage/Looks/Authored");
  const auto referenced = materials.find("/GraphStage/Looks/Referenced");
  Check(authored != materials.end() && referenced != materials.end(),
      "the stage's materials are" + keys);

  // What both say: half of an sRGB image placed by ND_place2d, a roughness
  // channel, and an emission colour authored in sRGB.
  const Lotus::PreviewSurface& surface = Lotus::Preview(authored->second);
  Check(surface.base_color_texture && surface.roughness_texture,
      "the authored material has no lookups");
  const Lotus::TextureInput& base = *surface.base_color_texture;
  Check(base.texture.ends_with("palette.png|sRGB") &&
            base.scale == std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.0F} &&
            base.st_transform != Lotus::TextureInput{}.st_transform,
      "the authored base colour is " + Describe(base));
  Check(surface.roughness_texture->texture.ends_with("rgba.png|raw") &&
            surface.roughness_texture->channel == 1,
      "the authored roughness is " + Describe(*surface.roughness_texture));
  ExpectValues("the authored emission", surface.emission,
      {Fold(std::pow((double{0.8F} + 0.055) / 1.055, 2.4)),
          Fold(std::pow((double{0.4F} + 0.055) / 1.055, 2.4)),
          Fold(std::pow((double{0.2F} + 0.055) / 1.055, 2.4))});
  Check(referenced->second == authored->second,
      "the referenced .mtlx document translates differently: base colour " +
          (Lotus::Preview(referenced->second).base_color_texture
                  ? Describe(*Lotus::Preview(referenced->second).base_color_texture)
                  : std::string("constant")) +
          " against " + Describe(base));
}

// The same for MaterialX's glTF PBR (docs/design/GLTF_PBR.md section 8.2).
void GltfStageRoundTrip() {
  const auto materials = StageMaterials("gltf-stage-v1.usda");
  std::string keys;
  for (const auto& [key, material] : materials) keys += " " + key;
  const auto authored = materials.find("/GltfStage/Looks/Authored");
  const auto referenced = materials.find("/GltfStage/Looks/Referenced");
  Check(authored != materials.end() && referenced != materials.end(),
      "the glTF stage's materials are" + keys);
  Check(authored->second.Model() == Lotus::ShadingModel::GltfPbr,
      "the authored glTF material has another model");
  // What both say: half of an sRGB image, packed roughness and metallic, an
  // emissive colour authored in sRGB with a strength, and MASK at 0.25.
  const Lotus::GltfPbr& surface = Lotus::Gltf(authored->second);
  Check(surface.base_color.texture && surface.roughness.texture && surface.metallic.texture,
      "the authored glTF material has no lookups");
  const Lotus::TextureInput& base = *surface.base_color.texture;
  Check(base.texture.ends_with("palette.png|sRGB") &&
            base.scale == std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.0F},
      "the authored glTF base colour is " + Describe(base));
  Check(surface.roughness.texture->texture.ends_with("rgba.png|raw") &&
            surface.roughness.texture->channel == 1 && surface.metallic.texture->channel == 2,
      "the authored glTF roughness is " + Describe(*surface.roughness.texture));
  ExpectValues("the authored glTF emissive", surface.emissive.value,
      {Fold(std::pow((double{0.8F} + 0.055) / 1.055, 2.4)),
          Fold(std::pow((double{0.4F} + 0.055) / 1.055, 2.4)),
          Fold(std::pow((double{0.2F} + 0.055) / 1.055, 2.4))});
  Check(surface.emissive_strength == 2.0F, "the authored glTF emissive strength is lost");
  Check(authored->second.geometry.opacity == 0.6F &&
            authored->second.geometry.opacity_threshold == 0.25F,
      "the authored glTF alpha is " + std::to_string(authored->second.geometry.opacity) +
          " at " + std::to_string(authored->second.geometry.opacity_threshold));
  Check(referenced->second == authored->second,
      "the referenced glTF .mtlx document translates differently: opacity " +
          std::to_string(referenced->second.geometry.opacity) + " at " +
          std::to_string(referenced->second.geometry.opacity_threshold) + ", " +
          (referenced->second.Model() == Lotus::ShadingModel::GltfPbr &&
                      Lotus::Gltf(referenced->second).base_color.texture
                  ? Describe(*Lotus::Gltf(referenced->second).base_color.texture)
                  : std::string("no base colour lookup")));
}

} // namespace

int main(int argc, char** argv) try {
  const bool dump = argc > 1 && std::string(argv[1]) == "--dump";
  ValuesAndChannels();
  Arithmetic();
  ImagesAndCoordinates();
  Normals();
  Diagnostics();
  CompoundEquivalence();
  Completeness();
  GltfPbr();
  Definitions(dump);
  StageRoundTrip();
  GltfStageRoundTrip();
  std::cout << "the MaterialX graph translation holds\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
