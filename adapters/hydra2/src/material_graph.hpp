// SPDX-License-Identifier: Apache-2.0
// The MaterialX graph reader (docs/design/MATERIALX_TRANSLATION.md): the
// node definitions the translator holds, and the folding of a node graph
// into a constant or one texture lookup with a per-channel scale and bias.
#pragma once

#include "material_translator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace HdLotusGraph {

enum class Type {
  Float,
  Color3,
  Color4,
  Vector2,
  Vector3,
  Vector4,
  Integer,
  Boolean,
  String,
  Filename,
};

// The component count of a numeric type; 0 for the others.
[[nodiscard]] std::size_t Width(Type type);
[[nodiscard]] const char* Name(Type type);

struct InputDef {
  TfToken name;
  Type type = Type::Float;
  // Whether the definition gives the input a value; one bound to a geometric
  // property by default has none.
  bool has_value = true;
  std::array<double, 4> value{};
  std::string text;
};

// What a node does, which the reader dispatches on.
enum class Family {
  Surface,
  Constant,
  Dot,
  Convert,
  Extract,
  Separate,
  Combine,
  Add,
  Subtract,
  Multiply,
  Divide,
  Min,
  Max,
  Power,
  Invert,
  Mix,
  Clamp,
  Absval,
  Sqrt,
  Floor,
  Ceil,
  Image,
  TiledImage,
  GltfImage,
  GltfColorImage,
  GltfNormalMap,
  Texcoord,
  GeomProp,
  Place2d,
  Rotate2d,
  NormalMap,
};

struct NodeDef {
  TfToken id;
  Family family = Family::Constant;
  // The output's type; the first output's for a node with several.
  Type type = Type::Float;
  std::vector<InputDef> inputs;

  [[nodiscard]] const InputDef* Find(const TfToken& name) const;
};

// Every definition, in a fixed order.
[[nodiscard]] const std::vector<NodeDef>& NodeDefs();
// The definition of `id`, or null when the table does not hold it.
[[nodiscard]] const NodeDef* FindNodeDef(const TfToken& id);

// A node's authored parameter, or null.
[[nodiscard]] const VtValue* Parameter(const HdMaterialNode2& node, const TfToken& name);
// The first connection of a node's input, or null.
[[nodiscard]] const HdMaterialConnection2* Connection(const HdMaterialNode2& node,
    const TfToken& input);
// A token parameter, authored as a token or a string; empty otherwise.
[[nodiscard]] TfToken ReadToken(const VtValue* value);
// A node's file input: resolved, or as authored when it does not resolve;
// empty when there is none.
[[nodiscard]] std::string ReadFile(const HdMaterialNode2& node);

// (m[0] s + m[1] t + m[2], m[3] s + m[4] t + m[5]).
using Affine2 = std::array<double, 6>;

// A translation with a stated difference, noted where it happened.
struct Note {
  SdfPath node;
  TfToken node_type;
  std::string detail;
};

// One output of a node graph.
struct Value {
  enum class Kind {
    // A graph this does not translate; the reader holds why.
    Failed,
    Constant,
    // One image's channels, each with its own scale and bias.
    Lookup,
    // A texture-coordinate set under an affine map, which only an image's
    // coordinates read.
    Coordinates,
  };
  Kind kind = Kind::Constant;
  // 1 (float) to 4 (color4 and vector4); 2 for coordinates.
  std::size_t width = 1;
  std::array<double, 4> constant{};

  // A lookup: component c is texel channel source[c] times that channel's
  // scale plus its bias, and that channel's fallback without an image.
  std::array<std::uint32_t, 4> source{0, 1, 2, 3};
  std::array<double, 4> scale{1.0, 1.0, 1.0, 1.0};
  std::array<double, 4> bias{};
  std::array<double, 4> fallback{0.0, 0.0, 0.0, 1.0};
  std::string file;
  // sRGB or raw.
  TfToken color_space;
  Lotus::TextureWrap wrap_s = Lotus::TextureWrap::Repeat;
  Lotus::TextureWrap wrap_t = Lotus::TextureWrap::Repeat;

  // A lookup's and coordinates' source: the mesh's coordinate set, the
  // value on a mesh without it, and the map from it. An empty name is no
  // set: the lookup reads at the fallback.
  std::string texcoords;
  std::array<float, 2> texcoord_fallback{0.0F, 0.0F};
  Affine2 transform{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};

  // An ND_normalmap's output: a tangent-space normal.
  bool normal_map = false;
  // The first substitution made on the way.
  std::optional<Note> note;

  [[nodiscard]] bool IsConstant() const {
    return kind == Kind::Constant;
  }
  [[nodiscard]] bool IsLookup() const {
    return kind == Kind::Lookup;
  }
};

[[nodiscard]] Value Constant(std::size_t width, const std::array<double, 4>& components);

// The values a surface input clamps a lookup into, which lets an
// ND_clamp at least as wide pass as the last node before it.
struct Range {
  double low = 0.0;
  double high = 1.0;
};

// A surface input as read.
struct Reading {
  // The input's value: its connection's, its authored constant, or its
  // definition's default. The default too when `diagnostic` reports a graph
  // or value this does not translate.
  Value value;
  bool connected = false;
  // A parameter or a connection names the input.
  bool authored = false;
  // The read stopped, and `value` is the definition's default.
  bool failed = false;
  // The node the connection reads, for a diagnostic about its value.
  SdfPath upstream;
  TfToken upstream_type;
  // What stopped the read, or the substitution it made; its `input` is left
  // to the caller.
  std::optional<HdLotusMaterialDiagnostic> diagnostic;
};

// The IR lookup of a value: a colour's three channels in order, or a
// scalar's one. Nothing, with why, for another shape or a non-finite
// coefficient.
[[nodiscard]] std::optional<Lotus::TextureInput> ToTextureInput(const Value& value,
    bool colour, std::string& why);

// `a` times `b` component by component, a scalar scaling every component.
// A failed value, with why, for two lookups or a scalar lookup spread over
// a colour.
[[nodiscard]] Value Product(const Value& a, const Value& b, std::string& why);

class GraphReader {
public:
  explicit GraphReader(const HdMaterialNetwork2& network);

  // Input `input` of the node at `path`, whose definition is `def`.
  [[nodiscard]] Reading Read(const SdfPath& path, const HdMaterialNode2& node,
      const NodeDef& def, const TfToken& input,
      const std::optional<Range>& range = std::nullopt);

private:
  struct Context;
  struct Stop {
    HdLotusMaterialDiagnosticKind kind;
    SdfPath node;
    TfToken node_type;
    std::string detail;
    int depth;
  };

  // A graph this deep is cyclic or beyond any authored network.
  static constexpr int kMaxDepth = 64;

  Value FailAt(HdLotusMaterialDiagnosticKind kind, const SdfPath& node,
      const TfToken& node_type, int depth, std::string detail);
  Value Fail(HdLotusMaterialDiagnosticKind kind, const Context& context,
      std::string detail);
  Value In(const Context& context, const char* name);
  Value ConstantIn(const Context& context, const char* name);
  Value CoordinatesIn(const Context& context, const char* name);
  std::optional<int> IntegerIn(const Context& context, const char* name);
  Value Output(const HdMaterialConnection2& connection, int depth);
  Value Binary(const Context& context, Family family, const Value& a, const Value& b);
  Value Unary(const Context& context, const Value& in);
  Value Convert(const Context& context);
  Value Component(const Context& context, const Value& in, std::size_t index);
  Value Combine(const Context& context);
  Value Mix(const Context& context);
  Value Clamp(const Context& context);
  Value Image(const Context& context, const Value& coordinates, bool periodic);
  Value TiledImage(const Context& context);
  Value GltfImage(const Context& context);
  Value Place(const Context& context, const Value& coordinates, const Value& pivot,
      const Value& scale, double rotate, const Value& offset, int order);
  Value Mapped(const Context& context, const Value& coordinates, const Affine2& map);
  Value NormalMap(const Context& context, const Value& in, const Value& scale);

  const HdMaterialNetwork2& network_;
  std::optional<Stop> stop_;
  std::optional<Range> range_;
};

} // namespace HdLotusGraph

PXR_NAMESPACE_CLOSE_SCOPE
