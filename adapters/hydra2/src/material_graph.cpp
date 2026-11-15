// SPDX-License-Identifier: Apache-2.0
#include "material_graph.hpp"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace HdLotusGraph {

namespace {

using Kind = HdLotusMaterialDiagnosticKind;

constexpr Type kValueTypes[] = {Type::Float, Type::Color3, Type::Color4,
    Type::Vector2, Type::Vector3, Type::Vector4};

bool IsColor(Type type) {
  return type == Type::Color3 || type == Type::Color4;
}

// A numeric input whose components all default to `value`.
InputDef Uniform(const char* name, Type type, double value) {
  return {TfToken(name), type, true, {value, value, value, value}, {}};
}

InputDef Components(const char* name, Type type, std::array<double, 4> value) {
  return {TfToken(name), type, true, value, {}};
}

// An input the definition binds to a geometric property.
InputDef Unbound(const char* name, Type type) {
  return {TfToken(name), type, false, {}, {}};
}

InputDef Text(const char* name, const char* text, Type type = Type::String) {
  return {TfToken(name), type, true, {}, text};
}

// ND_image's inputs after its file: where it reads and how.
void AddAddressing(std::vector<InputDef>& inputs) {
  inputs.push_back(Text("uaddressmode", "periodic"));
  inputs.push_back(Text("vaddressmode", "periodic"));
}

void AddSequence(std::vector<InputDef>& inputs) {
  inputs.push_back(Text("framerange", ""));
  inputs.push_back(Uniform("frameoffset", Type::Integer, 0.0));
  inputs.push_back(Text("frameendaction", "constant"));
}

// ND_gltf_image's placement inputs, with its operation order's default.
void AddPlacement(std::vector<InputDef>& inputs, double order) {
  inputs.push_back(Unbound("texcoord", Type::Vector2));
  inputs.push_back(Components("pivot", Type::Vector2, {0.0, 1.0, 0.0, 0.0}));
  inputs.push_back(Uniform("scale", Type::Vector2, 1.0));
  inputs.push_back(Uniform("rotate", Type::Float, 0.0));
  inputs.push_back(Uniform("offset", Type::Vector2, 0.0));
  inputs.push_back(Uniform("operationorder", Type::Integer, order));
  AddAddressing(inputs);
  inputs.push_back(Text("filtertype", "linear"));
}

// ND_standard_surface_surfaceshader 1.0.1.
std::vector<InputDef> StandardSurfaceInputs() {
  const auto f = [](const char* name, double value) {
    return Uniform(name, Type::Float, value);
  };
  const auto c = [](const char* name, double value) {
    return Uniform(name, Type::Color3, value);
  };
  return {f("base", 1.0), c("base_color", 0.8), f("diffuse_roughness", 0.0),
      f("metalness", 0.0), f("specular", 1.0), c("specular_color", 1.0),
      f("specular_roughness", 0.2), f("specular_IOR", 1.5),
      f("specular_anisotropy", 0.0), f("specular_rotation", 0.0),
      f("transmission", 0.0), c("transmission_color", 1.0),
      f("transmission_depth", 0.0), c("transmission_scatter", 0.0),
      f("transmission_scatter_anisotropy", 0.0), f("transmission_dispersion", 0.0),
      f("transmission_extra_roughness", 0.0), f("subsurface", 0.0),
      c("subsurface_color", 1.0), c("subsurface_radius", 1.0),
      f("subsurface_scale", 1.0), f("subsurface_anisotropy", 0.0), f("sheen", 0.0),
      c("sheen_color", 1.0), f("sheen_roughness", 0.3), f("coat", 0.0),
      c("coat_color", 1.0), f("coat_roughness", 0.1), f("coat_anisotropy", 0.0),
      f("coat_rotation", 0.0), f("coat_IOR", 1.5), Unbound("coat_normal", Type::Vector3),
      f("coat_affect_color", 0.0), f("coat_affect_roughness", 0.0),
      f("thin_film_thickness", 0.0), f("thin_film_IOR", 1.5), f("emission", 0.0),
      c("emission_color", 1.0), c("opacity", 1.0),
      Uniform("thin_walled", Type::Boolean, 0.0), Unbound("normal", Type::Vector3),
      Unbound("tangent", Type::Vector3)};
}

std::vector<NodeDef> BuildNodeDefs() {
  std::vector<NodeDef> defs;
  const auto add = [&](const std::string& id, Family family, Type type,
                       std::vector<InputDef> inputs) {
    defs.push_back({TfToken(id), family, type, std::move(inputs)});
  };
  add("ND_standard_surface_surfaceshader", Family::Surface, Type::Float,
      StandardSurfaceInputs());

  struct BinaryNode {
    const char* name;
    Family family;
    double in2;
  };
  constexpr BinaryNode kBinary[] = {{"add", Family::Add, 0.0},
      {"subtract", Family::Subtract, 0.0}, {"multiply", Family::Multiply, 1.0},
      {"divide", Family::Divide, 1.0}, {"min", Family::Min, 0.0},
      {"max", Family::Max, 0.0}, {"power", Family::Power, 1.0}};
  struct UnaryNode {
    const char* name;
    Family family;
  };
  constexpr UnaryNode kUnary[] = {{"absval", Family::Absval}, {"floor", Family::Floor},
      {"ceil", Family::Ceil}, {"sqrt", Family::Sqrt}};

  for (const Type type : kValueTypes) {
    const std::string name = Name(type);
    const bool scalar = type == Type::Float;
    add("ND_constant_" + name, Family::Constant, type, {Uniform("value", type, 0.0)});
    add("ND_dot_" + name, Family::Dot, type, {Uniform("in", type, 0.0), Text("note", "")});
    for (const BinaryNode& node : kBinary) {
      const std::string id = std::string("ND_") + node.name + "_" + name;
      add(id, node.family, type,
          {Uniform("in1", type, 0.0), Uniform("in2", type, node.in2)});
      if (!scalar) {
        add(id + "FA", node.family, type,
            {Uniform("in1", type, 0.0), Uniform("in2", Type::Float, node.in2)});
      }
    }
    add("ND_invert_" + name, Family::Invert, type,
        {Uniform("in", type, 0.0), Uniform("amount", type, 1.0)});
    add("ND_clamp_" + name, Family::Clamp, type,
        {Uniform("in", type, 0.0), Uniform("low", type, 0.0), Uniform("high", type, 1.0)});
    add("ND_mix_" + name, Family::Mix, type,
        {Uniform("fg", type, 0.0), Uniform("bg", type, 0.0),
            Uniform("mix", Type::Float, 0.0)});
    if (!scalar) {
      add("ND_invert_" + name + "FA", Family::Invert, type,
          {Uniform("in", type, 0.0), Uniform("amount", Type::Float, 1.0)});
      add("ND_clamp_" + name + "FA", Family::Clamp, type,
          {Uniform("in", type, 0.0), Uniform("low", Type::Float, 0.0),
              Uniform("high", Type::Float, 1.0)});
      add("ND_mix_" + name + "_" + name, Family::Mix, type,
          {Uniform("fg", type, 0.0), Uniform("bg", type, 0.0), Uniform("mix", type, 0.0)});
      add("ND_extract_" + name, Family::Extract, Type::Float,
          {Uniform("in", type, 0.0), Uniform("index", Type::Integer, 0.0)});
    }
    for (const UnaryNode& node : kUnary) {
      // MaterialX defines no square root of a colour.
      if (node.family == Family::Sqrt && IsColor(type)) continue;
      add(std::string("ND_") + node.name + "_" + name, node.family, type,
          {Uniform("in", type, 0.0)});
    }
    for (const Type to : kValueTypes) {
      if (to != type && to != Type::Float) {
        add("ND_convert_" + name + "_" + Name(to), Family::Convert, to,
            {Uniform("in", type, 0.0)});
      }
    }

    std::vector<InputDef> image{Text("file", "", Type::Filename), Text("layer", ""),
        Uniform("default", type, 0.0), Unbound("texcoord", Type::Vector2)};
    AddAddressing(image);
    image.push_back(Text("filtertype", "linear"));
    AddSequence(image);
    add("ND_image_" + name, Family::Image, type, std::move(image));

    std::vector<InputDef> tiled{Text("file", "", Type::Filename),
        Uniform("default", type, 0.0), Unbound("texcoord", Type::Vector2),
        Uniform("uvtiling", Type::Vector2, 1.0), Uniform("uvoffset", Type::Vector2, 0.0),
        Uniform("realworldimagesize", Type::Vector2, 1.0),
        Uniform("realworldtilesize", Type::Vector2, 1.0), Text("filtertype", "linear")};
    AddSequence(tiled);
    add("ND_tiledimage_" + name, Family::TiledImage, type, std::move(tiled));
  }

  add("ND_separate2_vector2", Family::Separate, Type::Float,
      {Uniform("in", Type::Vector2, 0.0)});
  add("ND_separate3_color3", Family::Separate, Type::Float,
      {Uniform("in", Type::Color3, 0.0)});
  add("ND_separate3_vector3", Family::Separate, Type::Float,
      {Uniform("in", Type::Vector3, 0.0)});
  add("ND_separate4_color4", Family::Separate, Type::Float,
      {Uniform("in", Type::Color4, 0.0)});
  add("ND_separate4_vector4", Family::Separate, Type::Float,
      {Uniform("in", Type::Vector4, 0.0)});

  const auto combine = [&](const std::string& id, Type type,
                           std::initializer_list<Type> parts) {
    std::vector<InputDef> inputs;
    for (const Type part : parts) {
      const std::string name = "in" + std::to_string(inputs.size() + 1);
      inputs.push_back(Uniform(name.c_str(), part, 0.0));
    }
    add(id, Family::Combine, type, std::move(inputs));
  };
  constexpr Type f = Type::Float;
  combine("ND_combine2_vector2", Type::Vector2, {f, f});
  combine("ND_combine2_color4CF", Type::Color4, {Type::Color3, f});
  combine("ND_combine2_vector4VF", Type::Vector4, {Type::Vector3, f});
  combine("ND_combine2_vector4VV", Type::Vector4, {Type::Vector2, Type::Vector2});
  combine("ND_combine3_color3", Type::Color3, {f, f, f});
  combine("ND_combine3_vector3", Type::Vector3, {f, f, f});
  combine("ND_combine4_color4", Type::Color4, {f, f, f, f});
  combine("ND_combine4_vector4", Type::Vector4, {f, f, f, f});

  // glTF's images. ND_gltf_colorimage and the colour-4 ND_gltf_image default
  // to the other operation order.
  struct GltfImage {
    const char* id;
    Type type;
    bool factor;
    double order;
  };
  constexpr GltfImage kGltfImages[] = {
      {"ND_gltf_image_float_float_1_0", Type::Float, true, 0.0},
      {"ND_gltf_image_color3_color3_1_0", Type::Color3, true, 0.0},
      {"ND_gltf_image_color4_color4_1_0", Type::Color4, true, 1.0},
      {"ND_gltf_image_vector3_vector3_1_0", Type::Vector3, false, 0.0}};
  for (const GltfImage& node : kGltfImages) {
    std::vector<InputDef> inputs{Text("file", "", Type::Filename)};
    if (node.factor) inputs.push_back(Uniform("factor", node.type, 1.0));
    inputs.push_back(Uniform("default", node.type, 0.0));
    AddPlacement(inputs, node.order);
    add(node.id, Family::GltfImage, node.type, std::move(inputs));
  }
  std::vector<InputDef> colorimage{Text("file", "", Type::Filename),
      Uniform("default", Type::Color4, 0.0)};
  AddPlacement(colorimage, 1.0);
  colorimage.push_back(Uniform("color", Type::Color4, 1.0));
  colorimage.push_back(Uniform("geomcolor", Type::Color4, 1.0));
  add("ND_gltf_colorimage", Family::GltfColorImage, Type::Color4, std::move(colorimage));
  std::vector<InputDef> normalmap{Text("file", "", Type::Filename),
      Components("default", Type::Vector3, {0.5, 0.5, 1.0, 0.0})};
  AddPlacement(normalmap, 0.0);
  add("ND_gltf_normalmap_vector3_1_0", Family::GltfNormalMap, Type::Vector3,
      std::move(normalmap));

  add("ND_texcoord_vector2", Family::Texcoord, Type::Vector2,
      {Uniform("index", Type::Integer, 0.0)});
  add("ND_geompropvalue_vector2", Family::GeomProp, Type::Vector2,
      {Text("geomprop", ""), Uniform("default", Type::Vector2, 0.0)});
  add("ND_place2d_vector2", Family::Place2d, Type::Vector2,
      {Uniform("texcoord", Type::Vector2, 0.0), Uniform("pivot", Type::Vector2, 0.0),
          Uniform("scale", Type::Vector2, 1.0), Uniform("rotate", Type::Float, 0.0),
          Uniform("offset", Type::Vector2, 0.0),
          Uniform("operationorder", Type::Integer, 0.0)});
  add("ND_rotate2d_vector2", Family::Rotate2d, Type::Vector2,
      {Uniform("in", Type::Vector2, 0.0), Uniform("amount", Type::Float, 0.0)});
  for (const auto& [id, scale] : {std::pair{"ND_normalmap_float", Type::Float},
           std::pair{"ND_normalmap_vector2", Type::Vector2}}) {
    add(id, Family::NormalMap, Type::Vector3,
        {Components("in", Type::Vector3, {0.5, 0.5, 1.0, 0.0}), Uniform("scale", scale, 1.0),
            Unbound("normal", Type::Vector3), Unbound("tangent", Type::Vector3),
            Unbound("bitangent", Type::Vector3)});
  }
  return defs;
}

// A value's components: a scalar, a vector or a colour of either precision,
// all finite.
struct Read {
  std::size_t width = 1;
  std::array<double, 4> components{};
};

std::optional<Read> ReadComponents(const VtValue& value) {
  Read read;
  if (value.IsHolding<float>()) {
    read.components[0] = value.UncheckedGet<float>();
  } else if (value.IsHolding<double>()) {
    read.components[0] = value.UncheckedGet<double>();
  } else if (value.IsHolding<int>()) {
    read.components[0] = value.UncheckedGet<int>();
  } else if (value.IsHolding<GfVec2f>() || value.IsHolding<GfVec2d>()) {
    read.width = 2;
    const GfVec2d vector = value.IsHolding<GfVec2f>()
                               ? GfVec2d(value.UncheckedGet<GfVec2f>())
                               : value.UncheckedGet<GfVec2d>();
    read.components = {vector[0], vector[1], 0.0, 0.0};
  } else if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec3d>()) {
    read.width = 3;
    const GfVec3d vector = value.IsHolding<GfVec3f>()
                               ? GfVec3d(value.UncheckedGet<GfVec3f>())
                               : value.UncheckedGet<GfVec3d>();
    read.components = {vector[0], vector[1], vector[2], 0.0};
  } else if (value.IsHolding<GfVec4f>() || value.IsHolding<GfVec4d>()) {
    read.width = 4;
    const GfVec4d vector = value.IsHolding<GfVec4f>()
                               ? GfVec4d(value.UncheckedGet<GfVec4f>())
                               : value.UncheckedGet<GfVec4d>();
    read.components = {vector[0], vector[1], vector[2], vector[3]};
  } else {
    return std::nullopt;
  }
  for (std::size_t c = 0; c < read.width; ++c) {
    if (!std::isfinite(read.components[c])) return std::nullopt;
  }
  return read;
}

// What an authored colour space means to a translation whose working space
// is linear Rec.709: MaterialX's names, and the names OpenUSD gives the same
// spaces.
enum class Space { Raw, Srgb, Unknown };

Space Classify(const TfToken& space) {
  const std::string& name = space.GetString();
  for (const char* raw : {"", "lin_rec709", "lin_rec709_scene", "raw", "data", "none"}) {
    if (name == raw) return Space::Raw;
  }
  for (const char* srgb : {"srgb_texture", "srgb_rec709_scene"}) {
    if (name == srgb) return Space::Srgb;
  }
  return Space::Unknown;
}

double SrgbToLinear(double value) {
  return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

TfToken ColorSpaceOf(const HdMaterialNode2& node, const std::string& input) {
  return ReadToken(Parameter(node, TfToken("colorSpace:" + input)));
}

Affine2 Compose(const Affine2& outer, const Affine2& inner) {
  return {outer[0] * inner[0] + outer[1] * inner[3],
      outer[0] * inner[1] + outer[1] * inner[4],
      outer[0] * inner[2] + outer[1] * inner[5] + outer[2],
      outer[3] * inner[0] + outer[4] * inner[3],
      outer[3] * inner[1] + outer[4] * inner[4],
      outer[3] * inner[2] + outer[4] * inner[5] + outer[5]};
}

Affine2 Scaling(double x, double y) {
  return {x, 0.0, 0.0, 0.0, y, 0.0};
}

Affine2 Translation(double x, double y) {
  return {1.0, 0.0, x, 0.0, 1.0, y};
}

// ND_rotate2d by `degrees`, as its implementations compute it.
Affine2 Rotation(double degrees) {
  const double radians = degrees * 3.14159265358979323846 / 180.0;
  const double sa = std::sin(radians), ca = std::cos(radians);
  return {ca, sa, 0.0, -sa, ca, 0.0};
}

// Component c of a lookup, mapped to a x + b.
void Apply(Value& lookup, std::size_t c, double a, double b) {
  const std::uint32_t channel = lookup.source[c];
  lookup.scale[channel] *= a;
  lookup.bias[channel] = lookup.bias[channel] * a + b;
  lookup.fallback[channel] = lookup.fallback[channel] * a + b;
}

bool SameImage(const Value& a, const Value& b) {
  return a.file == b.file && a.color_space == b.color_space && a.wrap_s == b.wrap_s &&
         a.wrap_t == b.wrap_t && a.texcoords == b.texcoords &&
         a.texcoord_fallback == b.texcoord_fallback && a.transform == b.transform;
}

void Inherit(Value& value, const Value& from) {
  if (!value.note) value.note = from.note;
}

Value Failed() {
  Value value;
  value.kind = Value::Kind::Failed;
  return value;
}

const char* FamilyName(Family family) {
  switch (family) {
  case Family::Add: return "a sum";
  case Family::Subtract: return "a difference";
  case Family::Multiply: return "a product";
  case Family::Divide: return "a quotient";
  case Family::Min: return "a minimum";
  case Family::Max: return "a maximum";
  case Family::Power: return "a power";
  default: return "a function";
  }
}

// a (op) b for the arithmetic families, or a failed value with its kind and
// why.
Value Arithmetic(Family family, const Value& a, const Value& b, Kind& kind,
    std::string& why) {
  using K = Value::Kind;
  kind = Kind::UnsupportedGraph;
  if (a.kind == K::Failed || b.kind == K::Failed) return Failed();
  if (a.normal_map || b.normal_map) {
    why = "arithmetic on a normal map";
    return Failed();
  }
  const auto component = [](const Value& value, std::size_t c) {
    return value.width == 1 ? value.constant[0] : value.constant[c];
  };

  if (a.IsConstant() && b.IsConstant()) {
    Value result = Constant(std::max(a.width, b.width), {});
    for (std::size_t c = 0; c < result.width; ++c) {
      const double x = component(a, c), y = component(b, c);
      double folded = 0.0;
      switch (family) {
      case Family::Add: folded = x + y; break;
      case Family::Subtract: folded = x - y; break;
      case Family::Multiply: folded = x * y; break;
      case Family::Divide: folded = y != 0.0 ? x / y : std::nan(""); break;
      case Family::Min: folded = std::min(x, y); break;
      case Family::Max: folded = std::max(x, y); break;
      case Family::Power: folded = std::pow(x, y); break;
      default: folded = std::nan(""); break;
      }
      if (!std::isfinite(folded)) {
        kind = Kind::UnsupportedValue;
        why = family == Family::Divide && y == 0.0
                  ? "division by a constant with a zero component"
                  : std::string(FamilyName(family)) + " that is not finite";
        return Failed();
      }
      result.constant[c] = folded;
    }
    Inherit(result, a);
    Inherit(result, b);
    return result;
  }

  const bool affine = family == Family::Add || family == Family::Subtract ||
                      family == Family::Multiply || family == Family::Divide;
  const bool first = !a.IsConstant();
  const Value& variable = first ? a : b;
  const Value& constant = first ? b : a;
  if (!constant.IsConstant()) {
    why = a.IsLookup() && b.IsLookup() ? "two lookups under one node"
                                        : "two texture-coordinate sources under one node";
    return Failed();
  }
  if (!affine) {
    why = std::string(FamilyName(family)) + " of a lookup is not a scale and a bias";
    return Failed();
  }
  if (!first && family == Family::Divide) {
    why = "a constant divided by a lookup is not a scale and a bias";
    return Failed();
  }
  if (variable.IsLookup() && variable.width == 1 && constant.width > 1) {
    why = "a scalar lookup spread over several components";
    return Failed();
  }
  // variable (op) constant, or constant (op) variable, as k x + m.
  std::array<double, 4> k{}, m{};
  for (std::size_t c = 0; c < variable.width; ++c) {
    const double y = component(constant, c);
    switch (family) {
    case Family::Add: k[c] = 1.0, m[c] = y; break;
    case Family::Subtract: k[c] = first ? 1.0 : -1.0, m[c] = first ? -y : y; break;
    case Family::Multiply: k[c] = y, m[c] = 0.0; break;
    default:
      if (y == 0.0) {
        kind = Kind::UnsupportedValue;
        why = "division by a constant with a zero component";
        return Failed();
      }
      k[c] = 1.0 / y, m[c] = 0.0;
      break;
    }
  }
  Value result = variable;
  if (result.IsLookup()) {
    for (std::size_t c = 0; c < result.width; ++c) Apply(result, c, k[c], m[c]);
  } else {
    result.transform = Compose({k[0], 0.0, m[0], 0.0, k[1], m[1]}, result.transform);
  }
  Inherit(result, constant);
  return result;
}

} // namespace

std::size_t Width(Type type) {
  switch (type) {
  case Type::Float: return 1;
  case Type::Vector2: return 2;
  case Type::Color3:
  case Type::Vector3: return 3;
  case Type::Color4:
  case Type::Vector4: return 4;
  default: return 0;
  }
}

const char* Name(Type type) {
  switch (type) {
  case Type::Float: return "float";
  case Type::Color3: return "color3";
  case Type::Color4: return "color4";
  case Type::Vector2: return "vector2";
  case Type::Vector3: return "vector3";
  case Type::Vector4: return "vector4";
  case Type::Integer: return "integer";
  case Type::Boolean: return "boolean";
  case Type::String: return "string";
  case Type::Filename: return "filename";
  }
  return "";
}

const InputDef* NodeDef::Find(const TfToken& name) const {
  for (const InputDef& input : inputs) {
    if (input.name == name) return &input;
  }
  return nullptr;
}

const std::vector<NodeDef>& NodeDefs() {
  static const std::vector<NodeDef> defs = BuildNodeDefs();
  return defs;
}

const NodeDef* FindNodeDef(const TfToken& id) {
  static const std::unordered_map<TfToken, const NodeDef*, TfToken::HashFunctor> index = [] {
    std::unordered_map<TfToken, const NodeDef*, TfToken::HashFunctor> built;
    for (const NodeDef& def : NodeDefs()) built.emplace(def.id, &def);
    return built;
  }();
  const auto found = index.find(id);
  return found == index.end() ? nullptr : found->second;
}

const VtValue* Parameter(const HdMaterialNode2& node, const TfToken& name) {
  // An empty value is an input authored without one.
  const auto parameter = node.parameters.find(name);
  return parameter == node.parameters.end() || parameter->second.IsEmpty()
             ? nullptr
             : &parameter->second;
}

const HdMaterialConnection2* Connection(const HdMaterialNode2& node,
    const TfToken& input) {
  const auto connected = node.inputConnections.find(input);
  return connected == node.inputConnections.end() || connected->second.empty()
             ? nullptr
             : &connected->second.front();
}

TfToken ReadToken(const VtValue* value) {
  if (value != nullptr && value->IsHolding<TfToken>()) {
    return value->UncheckedGet<TfToken>();
  }
  if (value != nullptr && value->IsHolding<std::string>()) {
    return TfToken(value->UncheckedGet<std::string>());
  }
  return {};
}

std::string ReadFile(const HdMaterialNode2& node) {
  if (const VtValue* asset = Parameter(node, TfToken("file"))) {
    if (asset->IsHolding<SdfAssetPath>()) {
      const SdfAssetPath& path = asset->UncheckedGet<SdfAssetPath>();
      return path.GetResolvedPath().empty() ? path.GetAssetPath()
                                            : path.GetResolvedPath();
    }
    if (asset->IsHolding<std::string>()) {
      return asset->UncheckedGet<std::string>();
    }
  }
  return {};
}

Value Constant(std::size_t width, const std::array<double, 4>& components) {
  Value value;
  value.width = width;
  value.constant = components;
  return value;
}

std::optional<Lotus::TextureInput> ToTextureInput(const Value& value, bool colour,
    std::string& why) {
  if (!value.IsLookup()) {
    why = "not a lookup";
    return std::nullopt;
  }
  Lotus::TextureInput input;
  if (colour) {
    if (value.width != 3 || value.source[0] != 0 || value.source[1] != 1 ||
        value.source[2] != 2) {
      why = "three components that are not one lookup's first three channels in order";
      return std::nullopt;
    }
  } else if (value.width != 1) {
    why = "a lookup of several components on a scalar";
    return std::nullopt;
  } else {
    input.channel = value.source[0];
  }
  input.texture = HdLotusTextureKey(value.file, value.color_space);
  input.wrap_s = value.wrap_s;
  input.wrap_t = value.wrap_t;
  bool finite = true;
  for (std::size_t c = 0; c < 4; ++c) {
    input.scale[c] = static_cast<float>(value.scale[c]);
    input.bias[c] = static_cast<float>(value.bias[c]);
    input.fallback[c] = static_cast<float>(value.fallback[c]);
    finite = finite && std::isfinite(input.scale[c]) && std::isfinite(input.bias[c]) &&
             std::isfinite(input.fallback[c]);
  }
  for (std::size_t i = 0; i < input.st_transform.size(); ++i) {
    input.st_transform[i] = static_cast<float>(value.transform[i]);
    finite = finite && std::isfinite(input.st_transform[i]);
  }
  if (!finite) {
    why = "a scale, bias or coordinate transform beyond a float";
    return std::nullopt;
  }
  return input;
}

Value Product(const Value& a, const Value& b, std::string& why) {
  Kind kind = Kind::UnsupportedGraph;
  return Arithmetic(Family::Multiply, a, b, kind, why);
}

struct GraphReader::Context {
  const SdfPath& path;
  const HdMaterialNode2& node;
  const NodeDef& def;
  int depth;
  // The output the connection reads.
  TfToken output;
};

GraphReader::GraphReader(const HdMaterialNetwork2& network) : network_(network) {
}

Value GraphReader::FailAt(Kind kind, const SdfPath& node, const TfToken& node_type,
    int depth, std::string detail) {
  // The node nearest the surface wins; the first, among equals.
  if (!stop_ || depth < stop_->depth) {
    stop_ = Stop{kind, node, node_type, std::move(detail), depth};
  }
  return Failed();
}

Value GraphReader::Fail(Kind kind, const Context& context, std::string detail) {
  return FailAt(kind, context.path, context.node.nodeTypeId, context.depth,
      std::move(detail));
}

Reading GraphReader::Read(const SdfPath& path, const HdMaterialNode2& node,
    const NodeDef& def, const TfToken& input, const std::optional<Range>& range) {
  Reading reading;
  stop_.reset();
  range_ = range;
  const HdMaterialConnection2* connection = Connection(node, input);
  reading.connected = connection != nullptr;
  reading.authored = reading.connected || Parameter(node, input) != nullptr;
  if (connection != nullptr) {
    reading.upstream = connection->upstreamNode;
    if (const auto upstream = network_.nodes.find(connection->upstreamNode);
        upstream != network_.nodes.end()) {
      reading.upstream_type = upstream->second.nodeTypeId;
    }
  }
  const InputDef* definition = def.Find(input);
  const Context context{path, node, def, 0, {}};
  reading.value = In(context, input.GetText());
  if (reading.value.kind == Value::Kind::Failed) {
    reading.failed = true;
    reading.value = Constant(std::max<std::size_t>(Width(definition->type), 1),
        definition->value);
    if (stop_) {
      reading.diagnostic = HdLotusMaterialDiagnostic{
          stop_->kind, {}, stop_->node, stop_->node_type, stop_->detail};
    }
  } else if (reading.value.note) {
    const Note& note = *reading.value.note;
    reading.diagnostic = HdLotusMaterialDiagnostic{
        Kind::Substituted, {}, note.node, note.node_type, note.detail};
  }
  return reading;
}

Value GraphReader::In(const Context& context, const char* name) {
  const TfToken token(name);
  const InputDef* input = context.def.Find(token);
  if (input == nullptr) {
    return Fail(Kind::UnsupportedGraph, context,
        std::string("the definition has no input ") + name);
  }
  const std::size_t width = Width(input->type);
  if (const HdMaterialConnection2* connection = Connection(context.node, token)) {
    Value value = Output(*connection, context.depth + 1);
    if (value.kind == Value::Kind::Failed) return value;
    if (value.width != width) {
      return Fail(Kind::UnsupportedGraph, context,
          std::string("input ") + name + " takes a " + Name(input->type) +
              ", and its connection has " + std::to_string(value.width) +
              (value.width == 1 ? " component" : " components"));
    }
    return value;
  }
  const VtValue* authored = Parameter(context.node, token);
  if (authored == nullptr) {
    return Constant(width, input->value);
  }
  const auto read = ReadComponents(*authored);
  if (!read || read->width != width) {
    return Fail(Kind::UnsupportedValue, context,
        std::string("input ") + name + " holds no finite " + Name(input->type));
  }
  Value value = Constant(width, read->components);
  if (IsColor(input->type)) {
    const TfToken space = ColorSpaceOf(context.node, name);
    switch (Classify(space)) {
    case Space::Raw: break;
    case Space::Srgb:
      for (std::size_t c = 0; c < 3; ++c) value.constant[c] = SrgbToLinear(value.constant[c]);
      break;
    case Space::Unknown:
      value.note = Note{context.path, context.node.nodeTypeId,
          std::string("input ") + name + "'s colour space " + space.GetString() +
              " is read as linear Rec.709"};
      break;
    }
  }
  return value;
}

Value GraphReader::ConstantIn(const Context& context, const char* name) {
  Value value = In(context, name);
  if (value.kind == Value::Kind::Failed) return value;
  if (!value.IsConstant() || value.normal_map) {
    return Fail(Kind::UnsupportedGraph, context,
        std::string("input ") + name + " is not a constant");
  }
  return value;
}

Value GraphReader::CoordinatesIn(const Context& context, const char* name) {
  const TfToken token(name);
  const InputDef* input = context.def.Find(token);
  if (input != nullptr && !input->has_value && Connection(context.node, token) == nullptr &&
      Parameter(context.node, token) == nullptr) {
    // The definition's default geometric property: the first coordinate set.
    Value value;
    value.kind = Value::Kind::Coordinates;
    value.width = 2;
    value.texcoords = "st";
    return value;
  }
  Value value = In(context, name);
  if (value.kind == Value::Kind::Failed || value.kind == Value::Kind::Coordinates ||
      value.IsConstant()) {
    return value;
  }
  return Fail(Kind::UnsupportedGraph, context,
      std::string("input ") + name + " reads texture coordinates from an image");
}

std::optional<int> GraphReader::IntegerIn(const Context& context, const char* name) {
  const TfToken token(name);
  const VtValue* authored = Parameter(context.node, token);
  if (authored == nullptr) {
    const InputDef* input = context.def.Find(token);
    return input != nullptr ? static_cast<int>(input->value[0]) : 0;
  }
  if (authored->IsHolding<int>()) return authored->UncheckedGet<int>();
  Fail(Kind::UnsupportedValue, context, std::string("input ") + name + " holds no integer");
  return std::nullopt;
}

Value GraphReader::Output(const HdMaterialConnection2& connection, int depth) {
  const SdfPath& path = connection.upstreamNode;
  const auto found = network_.nodes.find(path);
  if (found == network_.nodes.end()) {
    return FailAt(Kind::UnsupportedGraph, path, {}, depth, "the connection names no node");
  }
  const HdMaterialNode2& node = found->second;
  if (depth > kMaxDepth) {
    return FailAt(Kind::UnsupportedGraph, path, node.nodeTypeId, depth,
        "a graph deeper than " + std::to_string(kMaxDepth) + " nodes, or a cycle");
  }
  const NodeDef* def = FindNodeDef(node.nodeTypeId);
  if (def == nullptr || def->family == Family::Surface) {
    return FailAt(Kind::UnsupportedNode, path, node.nodeTypeId, depth,
        "the node table does not hold " + node.nodeTypeId.GetString());
  }
  const Context context{path, node, *def, depth, connection.upstreamOutputName};
  // Every authored input is one the definition names; a parameter with a
  // colon is Hydra's note about another (colorSpace:file).
  for (const auto& [name, value] : node.parameters) {
    if (name.GetString().find(':') == std::string::npos && def->Find(name) == nullptr) {
      return Fail(Kind::UnsupportedValue, context,
          "input " + name.GetString() + " is not in the node's definition");
    }
  }
  for (const auto& [name, connections] : node.inputConnections) {
    const InputDef* input = def->Find(name);
    if (input == nullptr) {
      return Fail(Kind::UnsupportedValue, context,
          "input " + name.GetString() + " is not in the node's definition");
    }
    if (Width(input->type) == 0) {
      return Fail(Kind::UnsupportedGraph, context,
          "a connected " + name.GetString());
    }
  }

  switch (def->family) {
  case Family::Surface: break;
  case Family::Constant: return In(context, "value");
  case Family::Dot: return In(context, "in");
  case Family::Convert: return Convert(context);
  case Family::Extract: {
    const Value in = In(context, "in");
    const auto index = IntegerIn(context, "index");
    if (in.kind == Value::Kind::Failed || !index) return Failed();
    if (*index < 0 || static_cast<std::size_t>(*index) >= in.width) {
      return Fail(Kind::UnsupportedValue, context,
          "index " + std::to_string(*index) + " of " + std::to_string(in.width) +
              " components");
    }
    return Component(context, in, static_cast<std::size_t>(*index));
  }
  case Family::Separate: {
    const Value in = In(context, "in");
    if (in.kind == Value::Kind::Failed) return in;
    const std::string& output = context.output.GetString();
    std::size_t index = std::string_view::npos;
    if (output.size() == 4 && output.starts_with("out")) {
      index = std::string_view("xyzw").find(output[3]);
      if (index == std::string_view::npos) index = std::string_view("rgba").find(output[3]);
    }
    if (index == std::string_view::npos || index >= in.width) {
      return Fail(Kind::UnsupportedGraph, context, "the node has no output " + output);
    }
    return Component(context, in, index);
  }
  case Family::Combine: return Combine(context);
  case Family::Add:
  case Family::Subtract:
  case Family::Multiply:
  case Family::Divide:
  case Family::Min:
  case Family::Max:
  case Family::Power: {
    const Value a = In(context, "in1");
    const Value b = In(context, "in2");
    return Binary(context, def->family, a, b);
  }
  case Family::Invert: {
    const Value in = In(context, "in");
    const Value amount = In(context, "amount");
    return Binary(context, Family::Subtract, amount, in);
  }
  case Family::Mix: return Mix(context);
  case Family::Clamp: return Clamp(context);
  case Family::Absval:
  case Family::Sqrt:
  case Family::Floor:
  case Family::Ceil: return Unary(context, In(context, "in"));
  case Family::Image:
    return Image(context, CoordinatesIn(context, "texcoord"), false);
  case Family::TiledImage: return TiledImage(context);
  case Family::GltfImage:
  case Family::GltfColorImage:
  case Family::GltfNormalMap: return GltfImage(context);
  case Family::Texcoord: {
    const auto index = IntegerIn(context, "index");
    if (!index) return Failed();
    if (*index != 0) {
      return Fail(Kind::UnsupportedValue, context,
          "texture-coordinate index " + std::to_string(*index) +
              ": only the first set is read");
    }
    Value value;
    value.kind = Value::Kind::Coordinates;
    value.width = 2;
    value.texcoords = "st";
    return value;
  }
  case Family::GeomProp: {
    const Value fallback = ConstantIn(context, "default");
    if (fallback.kind == Value::Kind::Failed) return fallback;
    Value value;
    value.kind = Value::Kind::Coordinates;
    value.width = 2;
    value.texcoords = ReadToken(Parameter(node, TfToken("geomprop"))).GetString();
    value.texcoord_fallback = {static_cast<float>(fallback.constant[0]),
        static_cast<float>(fallback.constant[1])};
    return value;
  }
  case Family::Place2d: {
    const Value coordinates = CoordinatesIn(context, "texcoord");
    const Value pivot = ConstantIn(context, "pivot");
    const Value scale = ConstantIn(context, "scale");
    const Value rotate = ConstantIn(context, "rotate");
    const Value offset = ConstantIn(context, "offset");
    const auto order = IntegerIn(context, "operationorder");
    for (const Value* value : {&coordinates, &pivot, &scale, &rotate, &offset}) {
      if (value->kind == Value::Kind::Failed) return Failed();
    }
    if (!order) return Failed();
    return Place(context, coordinates, pivot, scale, rotate.constant[0], offset, *order);
  }
  case Family::Rotate2d: {
    const Value in = CoordinatesIn(context, "in");
    const Value amount = ConstantIn(context, "amount");
    if (in.kind == Value::Kind::Failed || amount.kind == Value::Kind::Failed) {
      return Failed();
    }
    return Mapped(context, in, Rotation(amount.constant[0]));
  }
  case Family::NormalMap: {
    const Value in = In(context, "in");
    const Value scale = ConstantIn(context, "scale");
    return NormalMap(context, in, scale);
  }
  }
  return Fail(Kind::UnsupportedNode, context, "the node table does not hold it");
}

Value GraphReader::Binary(const Context& context, Family family, const Value& a,
    const Value& b) {
  Kind kind = Kind::UnsupportedGraph;
  std::string why;
  Value result = Arithmetic(family, a, b, kind, why);
  if (result.kind == Value::Kind::Failed && !why.empty()) {
    return Fail(kind, context, why);
  }
  return result;
}

Value GraphReader::Unary(const Context& context, const Value& in) {
  if (in.kind == Value::Kind::Failed) return in;
  if (!in.IsConstant() || in.normal_map) {
    return Fail(Kind::UnsupportedGraph, context,
        "a function of a lookup that is not a scale and a bias");
  }
  Value result = in;
  for (std::size_t c = 0; c < result.width; ++c) {
    double& value = result.constant[c];
    switch (context.def.family) {
    case Family::Absval: value = std::abs(value); break;
    case Family::Sqrt: value = std::sqrt(value); break;
    case Family::Floor: value = std::floor(value); break;
    default: value = std::ceil(value); break;
    }
    if (!std::isfinite(value)) {
      return Fail(Kind::UnsupportedValue, context, "a result that is not finite");
    }
  }
  return result;
}

// ND_convert as its library graphs do: a scalar spreads; a narrower type
// keeps the first components; a wider one gains 0, and 1 in the fourth.
Value GraphReader::Convert(const Context& context) {
  Value value = In(context, "in");
  if (value.kind == Value::Kind::Failed) return value;
  const std::size_t from = value.width, to = Width(context.def.type);
  if (value.kind == Value::Kind::Coordinates || value.normal_map) {
    return Fail(Kind::UnsupportedGraph, context,
        "a conversion of texture coordinates or a normal map");
  }
  if (value.IsConstant()) {
    for (std::size_t c = from; c < to; ++c) {
      value.constant[c] = from == 1 ? value.constant[0] : c == 3 ? 1.0 : 0.0;
    }
    value.width = to;
    return value;
  }
  if (from == 1) {
    return Fail(Kind::UnsupportedGraph, context,
        "a scalar lookup spread over several components");
  }
  if (from < 3 || to < 3) {
    return Fail(Kind::UnsupportedGraph, context,
        "a lookup converts only between three- and four-component types");
  }
  if (to == 4 && from == 3) {
    // The fourth component is 1, whatever the image's fourth channel holds.
    std::array<bool, 4> used{};
    for (std::size_t c = 0; c < 3; ++c) used[value.source[c]] = true;
    const auto free = static_cast<std::uint32_t>(
        std::find(used.begin(), used.end(), false) - used.begin());
    value.source[3] = free;
    value.scale[free] = 0.0;
    value.bias[free] = 1.0;
    value.fallback[free] = 1.0;
  }
  value.width = to;
  return value;
}

Value GraphReader::Component(const Context& context, const Value& in, std::size_t index) {
  if (in.kind == Value::Kind::Coordinates || in.normal_map) {
    return Fail(Kind::UnsupportedGraph, context,
        "a component of texture coordinates or a normal map");
  }
  Value result = in;
  result.width = 1;
  if (in.IsConstant()) {
    result.constant = {in.constant[index], 0.0, 0.0, 0.0};
  } else {
    result.source[0] = in.source[index];
  }
  return result;
}

// Constants, or one lookup's channels 0, 1, 2 (and 3) in order, each with
// its own scale and bias.
Value GraphReader::Combine(const Context& context) {
  std::vector<Value> parts;
  bool failed = false, constant = true, lookups = true;
  for (const InputDef& input : context.def.inputs) {
    parts.push_back(In(context, input.name.GetText()));
    const Value& part = parts.back();
    failed = failed || part.kind == Value::Kind::Failed;
    constant = constant && part.IsConstant() && !part.normal_map;
    lookups = lookups && part.IsLookup() && !part.normal_map;
  }
  if (failed) return Failed();
  if (constant) {
    Value result = Constant(0, {});
    for (const Value& part : parts) {
      for (std::size_t c = 0; c < part.width; ++c) {
        result.constant[result.width++] = part.constant[c];
      }
      Inherit(result, part);
    }
    return result;
  }
  const char* why = "components that are neither all constants nor one lookup's "
                    "channels in order";
  if (!lookups) return Fail(Kind::UnsupportedGraph, context, why);
  Value result = parts.front();
  result.width = 0;
  for (const Value& part : parts) {
    if (!SameImage(part, parts.front())) return Fail(Kind::UnsupportedGraph, context, why);
    for (std::size_t c = 0; c < part.width; ++c) {
      const std::uint32_t channel = part.source[c];
      if (channel != result.width) return Fail(Kind::UnsupportedGraph, context, why);
      result.source[channel] = channel;
      result.scale[channel] = part.scale[channel];
      result.bias[channel] = part.bias[channel];
      result.fallback[channel] = part.fallback[channel];
      ++result.width;
    }
    Inherit(result, part);
  }
  return result;
}

// bg (1 - mix) + fg mix, with a constant mix.
Value GraphReader::Mix(const Context& context) {
  const Value fg = In(context, "fg");
  const Value bg = In(context, "bg");
  const Value mix = In(context, "mix");
  if (fg.kind == Value::Kind::Failed || bg.kind == Value::Kind::Failed ||
      mix.kind == Value::Kind::Failed) {
    return Failed();
  }
  if (!mix.IsConstant()) {
    return Fail(Kind::UnsupportedGraph, context, "a mix weight that is not a constant");
  }
  Value rest = mix;
  for (std::size_t c = 0; c < rest.width; ++c) rest.constant[c] = 1.0 - mix.constant[c];
  const Value back = Binary(context, Family::Multiply, bg, rest);
  const Value front = Binary(context, Family::Multiply, fg, mix);
  return Binary(context, Family::Add, back, front);
}

// Constants fold. A lookup's clamp passes only as the last node before a
// surface input whose own range lies inside it, where it changes nothing.
Value GraphReader::Clamp(const Context& context) {
  const Value in = In(context, "in");
  const Value low = In(context, "low");
  const Value high = In(context, "high");
  if (in.kind == Value::Kind::Failed || low.kind == Value::Kind::Failed ||
      high.kind == Value::Kind::Failed) {
    return Failed();
  }
  if (in.kind == Value::Kind::Coordinates || in.normal_map) {
    return Fail(Kind::UnsupportedGraph, context,
        "a clamp of texture coordinates or a normal map");
  }
  if (!low.IsConstant() || !high.IsConstant()) {
    return Fail(Kind::UnsupportedGraph, context, "a clamp bound that is not a constant");
  }
  const auto bound = [](const Value& value, std::size_t c) {
    return value.width == 1 ? value.constant[0] : value.constant[c];
  };
  Value result = in;
  if (in.IsConstant()) {
    for (std::size_t c = 0; c < in.width; ++c) {
      result.constant[c] = std::min(std::max(in.constant[c], bound(low, c)), bound(high, c));
    }
    return result;
  }
  bool inside = context.depth == 1 && range_.has_value();
  for (std::size_t c = 0; inside && c < in.width; ++c) {
    inside = bound(low, c) <= range_->low && bound(high, c) >= range_->high;
  }
  if (!inside) {
    return Fail(Kind::UnsupportedGraph, context,
        "a clamp of a lookup that the surface input's own range does not contain");
  }
  return result;
}

// An image node's lookup at `coordinates`, or its default without a file.
Value GraphReader::Image(const Context& context, const Value& coordinates, bool periodic) {
  const Value fallback = ConstantIn(context, "default");
  if (coordinates.kind == Value::Kind::Failed || fallback.kind == Value::Kind::Failed) {
    return Failed();
  }
  const auto text = [&](const char* name) {
    const TfToken token(name);
    if (const VtValue* authored = Parameter(context.node, token)) {
      return ReadToken(authored).GetString();
    }
    const InputDef* input = context.def.Find(token);
    return input != nullptr ? input->text : std::string();
  };
  if (!text("layer").empty()) {
    return Fail(Kind::UnsupportedValue, context, "an image layer");
  }
  if (!text("framerange").empty()) {
    return Fail(Kind::UnsupportedValue, context, "an image sequence");
  }
  const std::string file = ReadFile(context.node);
  if (file.empty()) {
    return fallback;
  }

  const std::size_t width = Width(context.def.type);
  Value value;
  value.kind = Value::Kind::Lookup;
  value.width = width;
  value.fallback = {0.0, 0.0, 0.0, width == 4 ? 0.0 : 1.0};
  for (std::size_t c = 0; c < width; ++c) value.fallback[c] = fallback.constant[c];
  if (width == 1) {
    value.fallback = {fallback.constant[0], fallback.constant[0], fallback.constant[0], 1.0};
  }
  value.note = fallback.note;
  Inherit(value, coordinates);
  value.file = file;
  if (coordinates.kind == Value::Kind::Coordinates) {
    value.texcoords = coordinates.texcoords;
    value.texcoord_fallback = coordinates.texcoord_fallback;
    value.transform = coordinates.transform;
  } else {
    // A constant point: no coordinate set, and the point on every mesh.
    value.texcoord_fallback = {static_cast<float>(coordinates.constant[0]),
        static_cast<float>(coordinates.constant[1])};
  }
  const auto note = [&](std::string detail) {
    if (!value.note) {
      value.note = Note{context.path, context.node.nodeTypeId, std::move(detail)};
    }
  };

  if (!periodic) {
    for (const auto& [name, wrap] : {std::pair{"uaddressmode", &value.wrap_s},
             std::pair{"vaddressmode", &value.wrap_t}}) {
      const std::string mode = text(name);
      if (mode == "periodic") {
        *wrap = Lotus::TextureWrap::Repeat;
      } else if (mode == "clamp") {
        *wrap = Lotus::TextureWrap::Clamp;
      } else if (mode == "mirror") {
        *wrap = Lotus::TextureWrap::Mirror;
      } else if (mode == "constant") {
        *wrap = Lotus::TextureWrap::Black;
      } else {
        return Fail(Kind::UnsupportedValue, context,
            std::string(name) + " " + mode + " is no address mode");
      }
    }
    const bool black = value.wrap_s == Lotus::TextureWrap::Black ||
                       value.wrap_t == Lotus::TextureWrap::Black;
    const bool zero = std::all_of(fallback.constant.begin(),
        fallback.constant.begin() + static_cast<std::ptrdiff_t>(width),
        [](double component) { return component == 0.0; });
    if (black && !zero) {
      note("constant addressing reads black outside the image, not its default");
    }
  }
  const std::string filter = text("filtertype");
  if (filter == "closest" || filter == "cubic") {
    note("filtertype " + filter + " is read as linear");
  } else if (filter != "linear") {
    return Fail(Kind::UnsupportedValue, context, "filtertype " + filter + " is no filter");
  }
  // Only a colour image has a colour space.
  value.color_space = TfToken("raw");
  if (IsColor(context.def.type)) {
    const TfToken space = ColorSpaceOf(context.node, "file");
    switch (Classify(space)) {
    case Space::Raw: break;
    case Space::Srgb: value.color_space = TfToken("sRGB"); break;
    case Space::Unknown:
      note("the image's colour space " + space.GetString() + " is read as raw");
      break;
    }
  }
  return value;
}

// ND_tiledimage: periodic at uv x uvtiling - uvoffset.
Value GraphReader::TiledImage(const Context& context) {
  const Value coordinates = CoordinatesIn(context, "texcoord");
  const Value tiling = ConstantIn(context, "uvtiling");
  const Value offset = ConstantIn(context, "uvoffset");
  const Value image_size = ConstantIn(context, "realworldimagesize");
  const Value tile_size = ConstantIn(context, "realworldtilesize");
  for (const Value* value : {&coordinates, &tiling, &offset, &image_size, &tile_size}) {
    if (value->kind == Value::Kind::Failed) return Failed();
  }
  for (const Value* size : {&image_size, &tile_size}) {
    if (size->constant[0] != 1.0 || size->constant[1] != 1.0) {
      return Fail(Kind::UnsupportedValue, context, "real-world sizes are not translated");
    }
  }
  const Value placed = Mapped(context, coordinates,
      {tiling.constant[0], 0.0, -offset.constant[0], 0.0, tiling.constant[1],
          -offset.constant[1]});
  return Image(context, placed, true);
}

// ND_gltf_image, ND_gltf_colorimage and ND_gltf_normalmap as their library
// graphs: an image at ND_place2d with the scale inverted, the rotation
// negated and the offset negated in u.
Value GraphReader::GltfImage(const Context& context) {
  const Family family = context.def.family;
  const Value coordinates = CoordinatesIn(context, "texcoord");
  const Value pivot = ConstantIn(context, "pivot");
  const Value scale = ConstantIn(context, "scale");
  const Value rotate = ConstantIn(context, "rotate");
  const Value offset = ConstantIn(context, "offset");
  // ND_gltf_colorimage's graph fixes the order and periodic addressing,
  // whatever its own inputs say.
  const bool fixed = family == Family::GltfColorImage;
  const std::optional<int> order = fixed ? 0 : IntegerIn(context, "operationorder");
  for (const Value* value : {&coordinates, &pivot, &scale, &rotate, &offset}) {
    if (value->kind == Value::Kind::Failed) return Failed();
  }
  if (!order) return Failed();
  if (scale.constant[0] == 0.0 || scale.constant[1] == 0.0) {
    return Fail(Kind::UnsupportedValue, context, "a scale with a zero component");
  }
  const Value inverse = Constant(2, {1.0 / scale.constant[0], 1.0 / scale.constant[1]});
  const Value negated = Constant(2, {-offset.constant[0], offset.constant[1]});
  const Value placed =
      Place(context, coordinates, pivot, inverse, -rotate.constant[0], negated, *order);
  Value image = Image(context, placed, fixed);
  if (image.kind == Value::Kind::Failed) return image;

  if (family == Family::GltfNormalMap) {
    return NormalMap(context, image, Constant(1, {1.0}));
  }
  if (family == Family::GltfImage) {
    return context.def.Find(TfToken("factor")) != nullptr
               ? Binary(context, Family::Multiply, In(context, "factor"), image)
               : image;
  }
  const Value colored = Binary(context, Family::Multiply, In(context, "color"), image);
  const Value tinted =
      Binary(context, Family::Multiply, colored, ConstantIn(context, "geomcolor"));
  if (tinted.kind == Value::Kind::Failed) return tinted;
  if (context.output == TfToken("outa")) {
    return Component(context, tinted, 3);
  }
  if (context.output != TfToken("outcolor")) {
    return Fail(Kind::UnsupportedGraph, context,
        "the node has no output " + context.output.GetString());
  }
  Value color = tinted;
  color.width = 3;
  return color;
}

// ND_place2d as its library graph: scale, rotate, translate about the pivot
// (order 0), or translate, rotate, scale (order 1).
Value GraphReader::Place(const Context& context, const Value& coordinates,
    const Value& pivot, const Value& scale, double rotate, const Value& offset,
    int order) {
  if (order != 0 && order != 1) {
    return Fail(Kind::UnsupportedValue, context,
        "operationorder " + std::to_string(order) + " is neither 0 nor 1");
  }
  if (scale.constant[0] == 0.0 || scale.constant[1] == 0.0) {
    return Fail(Kind::UnsupportedValue, context, "a scale with a zero component");
  }
  const Affine2 to_pivot = Translation(-pivot.constant[0], -pivot.constant[1]);
  const Affine2 from_pivot = Translation(pivot.constant[0], pivot.constant[1]);
  const Affine2 divide = Scaling(1.0 / scale.constant[0], 1.0 / scale.constant[1]);
  const Affine2 turn = Rotation(rotate);
  const Affine2 shift = Translation(-offset.constant[0], -offset.constant[1]);
  const Affine2 map =
      order == 0 ? Compose(from_pivot, Compose(shift, Compose(turn, Compose(divide, to_pivot))))
                 : Compose(from_pivot, Compose(divide, Compose(turn, Compose(shift, to_pivot))));
  return Mapped(context, coordinates, map);
}

Value GraphReader::Mapped(const Context& context, const Value& coordinates,
    const Affine2& map) {
  Value result = coordinates;
  if (coordinates.kind == Value::Kind::Coordinates) {
    result.transform = Compose(map, coordinates.transform);
  } else if (coordinates.IsConstant() && coordinates.width == 2) {
    const double s = coordinates.constant[0], t = coordinates.constant[1];
    result.constant = {map[0] * s + map[1] * t + map[2], map[3] * s + map[4] * t + map[5],
        0.0, 0.0};
  } else {
    return Fail(Kind::UnsupportedGraph, context,
        "texture coordinates that are neither a coordinate set nor a constant");
  }
  return result;
}

// ND_normalmap: the unsigned tangent-space normal, signed, its x and y
// scaled, in the default tangent frame.
Value GraphReader::NormalMap(const Context& context, const Value& in, const Value& scale) {
  if (in.kind == Value::Kind::Failed || scale.kind == Value::Kind::Failed) {
    return Failed();
  }
  for (const char* frame : {"normal", "tangent", "bitangent"}) {
    const TfToken token(frame);
    if (context.def.family == Family::NormalMap &&
        (Parameter(context.node, token) != nullptr ||
            Connection(context.node, token) != nullptr)) {
      return Fail(Kind::UnsupportedGraph, context,
          std::string("an authored ") + frame + ": only the default tangent frame is read");
    }
  }
  if (in.kind == Value::Kind::Coordinates || in.normal_map || in.width != 3 ||
      (in.IsLookup() && (in.source[0] != 0 || in.source[1] != 1 || in.source[2] != 2))) {
    return Fail(Kind::UnsupportedGraph, context,
        "a normal map of something other than three channels in order");
  }
  const std::array<double, 3> factor{scale.constant[0],
      scale.width == 2 ? scale.constant[1] : scale.constant[0], 1.0};
  Value result = in;
  result.normal_map = true;
  for (std::size_t c = 0; c < 3; ++c) {
    if (result.IsLookup()) {
      Apply(result, c, 2.0 * factor[c], -factor[c]);
    } else {
      result.constant[c] = (2.0 * result.constant[c] - 1.0) * factor[c];
    }
  }
  Inherit(result, scale);
  return result;
}

} // namespace HdLotusGraph

PXR_NAMESPACE_CLOSE_SCOPE
