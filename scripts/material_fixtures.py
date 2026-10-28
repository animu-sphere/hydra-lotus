#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write or check the committed UsdPreviewSurface material fixtures.

Each fixture is authored here as USD text and PNG images, with no OpenUSD
or imaging dependency, and mirrored as a Lotus scene in
adapters/common/material_fixtures.cpp. The Hydra fixture test requires the
two to agree; the headless check renders the mirror against the committed
reference images.
"""

import argparse
import math
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "validation" / "materials"

# The unit sphere's tessellation: segments around +Y, rings pole to pole.
SEGMENTS = 32
RINGS = 16


def fixed(value):
    """`value` rounded to a multiple of 1e-6, as the C++ mirror rounds it."""
    units = round(value * 1_000_000)
    sign = "-" if units < 0 else ""
    units = abs(units)
    return f"{sign}{units // 1_000_000}.{units % 1_000_000:06d}"


def sphere(segments=SEGMENTS, rings=RINGS):
    """Points, face vertex counts and indices of the unit sphere.

    Ring i (1..rings-1) is at polar angle pi*i/rings; segment j at azimuth
    2*pi*j/segments, with x = sin(theta)cos(phi) and z = -sin(theta)sin(phi).
    Faces wind counter-clockwise seen from outside: triangles at the poles,
    quads between rings.
    """
    points = [(0.0, 1.0, 0.0)]
    for i in range(1, rings):
        theta = math.pi * i / rings
        for j in range(segments):
            phi = 2 * math.pi * j / segments
            points.append((math.sin(theta) * math.cos(phi), math.cos(theta),
                           -math.sin(theta) * math.sin(phi)))
    points.append((0.0, -1.0, 0.0))
    bottom = len(points) - 1

    def ring(i, j):
        return 1 + (i - 1) * segments + j % segments

    counts, indices = [], []
    for j in range(segments):
        counts.append(3)
        indices += [0, ring(1, j), ring(1, j + 1)]
    for i in range(1, rings - 1):
        for j in range(segments):
            counts.append(4)
            indices += [ring(i, j), ring(i + 1, j), ring(i + 1, j + 1), ring(i, j + 1)]
    for j in range(segments):
        counts.append(3)
        indices += [ring(rings - 1, j), bottom, ring(rings - 1, j + 1)]
    return points, counts, indices


def vectors(values):
    return ", ".join("(" + ", ".join(fixed(c) for c in v) + ")" for v in values)


def integers(values):
    return ", ".join(str(v) for v in values)


# material-basics-v1: the policy's first two audit items, base colour and
# metallic/roughness, on a 4x4 grid of spheres. Rows from the top: base
# colours, dielectric roughness, metal roughness, then partial metallic,
# metallic-workflow ior and the specular workflow. Each entry is the ball's
# name and its UsdPreviewSurface inputs, as authored; out-of-range and
# ignored values are deliberate.
BASICS = [
    [("White", {"diffuseColor": (0.8, 0.8, 0.8)}),
     ("Red", {"diffuseColor": (0.8, 0.1, 0.05)}),
     ("Dark", {"diffuseColor": (0.04, 0.04, 0.04)}),
     ("Clamped", {"diffuseColor": (1.25, 0.5, -0.25), "roughness": 1.5})],
    [("Gloss0", {"diffuseColor": (0.5, 0.5, 0.5), "roughness": 0.0}),
     ("Gloss25", {"diffuseColor": (0.5, 0.5, 0.5), "roughness": 0.25}),
     ("Gloss50", {"diffuseColor": (0.5, 0.5, 0.5), "roughness": 0.5}),
     ("Gloss100", {"diffuseColor": (0.5, 0.5, 0.5), "roughness": 1.0})],
    [("Metal0", {"diffuseColor": (0.95, 0.64, 0.54), "metallic": 1.0, "roughness": 0.0}),
     ("Metal25", {"diffuseColor": (0.95, 0.64, 0.54), "metallic": 1.0, "roughness": 0.25}),
     ("Metal50", {"diffuseColor": (0.95, 0.64, 0.54), "metallic": 1.0, "roughness": 0.5}),
     ("Metal100", {"diffuseColor": (0.95, 0.64, 0.54), "metallic": 1.0, "roughness": 1.0})],
    [("Metallic25", {"diffuseColor": (0.9, 0.6, 0.2), "metallic": 0.25, "roughness": 0.3}),
     ("Metallic75", {"diffuseColor": (0.9, 0.6, 0.2), "metallic": 0.75, "roughness": 0.3}),
     ("Ior25", {"diffuseColor": (0.1, 0.2, 0.6), "roughness": 0.1, "ior": 2.5}),
     ("Specular", {"diffuseColor": (0.05, 0.05, 0.05), "roughness": 0.2,
                   "useSpecularWorkflow": 1, "specularColor": (0.9, 0.6, 0.3),
                   "metallic": 1.0, "ior": 3.0})],
]
# Ball centres: columns left to right, rows top to bottom.
COLUMNS = (-1.35, -0.45, 0.45, 1.35)
ROWS = (1.35, 0.45, -0.45, -1.35)


def number(value):
    return repr(float(value)).removesuffix(".0") if float(value).is_integer() else repr(value)


def shader_input(name, value):
    if name == "useSpecularWorkflow":
        return f"int inputs:{name} = {value}"
    if isinstance(value, tuple):
        return f"color3f inputs:{name} = ({', '.join(number(c) for c in value)})"
    return f"float inputs:{name} = {number(value)}"


def basics():
    points, counts, indices = sphere()
    lines = [
        "#usda 1.0",
        "(",
        '    defaultPrim = "MaterialBasics"',
        "    metersPerUnit = 1",
        '    upAxis = "Y"',
        ")",
        "",
        "# Generated by scripts/material_fixtures.py; edit it there. Mirrored by",
        "# adapters/common/material_fixtures.cpp (SetMaterialBasics).",
        "",
        "# The unit sphere every ball inherits, with its authored vertex normals.",
        'class Mesh "_Ball"',
        "{",
        f"    int[] faceVertexCounts = [{integers(counts)}]",
        f"    int[] faceVertexIndices = [{integers(indices)}]",
        f"    normal3f[] normals = [{vectors(points)}]",
        f"    point3f[] points = [{vectors(points)}]",
        '    uniform token subdivisionScheme = "none"',
        "}",
        "",
        'def Xform "MaterialBasics"',
        "{",
        '    def Camera "Camera"',
        "    {",
        "        float2 clippingRange = (1, 100)",
        "        float focalLength = 50",
        "        float horizontalAperture = 32",
        "        float verticalAperture = 32",
        "        double3 xformOp:translate = (0, 0, 6.5)",
        '        uniform token[] xformOpOrder = ["xformOp:translate"]',
        "    }",
        "",
        '    def DomeLight "Dome"',
        "    {",
        "        float inputs:intensity = 0.5",
        "    }",
        "",
        '    def RectLight "Key"',
        "    {",
        "        float inputs:height = 3",
        "        float inputs:intensity = 4",
        "        float inputs:width = 3",
        "        float xformOp:rotateX = -30",
        "        float xformOp:rotateY = 20",
        "        double3 xformOp:translate = (1.5, 2.5, 4)",
        '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY", "xformOp:rotateX"]',
        "    }",
        "",
        "    # Unbound: lit by its displayColor.",
        '    def Mesh "Backdrop"',
        "    {",
        "        int[] faceVertexCounts = [4]",
        "        int[] faceVertexIndices = [0, 1, 2, 3]",
        "        point3f[] points = [(-3, -3, -1), (3, -3, -1), (3, 3, -1), (-3, 3, -1)]",
        "        color3f[] primvars:displayColor = [(0.5, 0.5, 0.5)]",
        '        uniform token subdivisionScheme = "none"',
        "    }",
        "",
        '    def Scope "Looks"',
        "    {",
    ]
    names = [name for row in BASICS for name, _ in row]
    for row in BASICS:
        for name, inputs in row:
            lines += [
                f'        def Material "{name}"',
                "        {",
                f"            token outputs:surface.connect = </MaterialBasics/Looks/{name}/Surface.outputs:surface>",
                "",
                '            def Shader "Surface"',
                "            {",
                '                uniform token info:id = "UsdPreviewSurface"',
            ]
            lines += [f"                {shader_input(k, inputs[k])}" for k in sorted(inputs)]
            lines += ["                token outputs:surface", "            }", "        }"]
            if name != names[-1]:
                lines.append("")
    lines += ["    }", "", '    def Scope "Balls"', "    {"]
    for y, row in zip(ROWS, BASICS):
        for x, (name, _) in zip(COLUMNS, row):
            lines += [
                f'        def Mesh "{name}" (',
                '            prepend apiSchemas = ["MaterialBindingAPI"]',
                "            prepend inherits = </_Ball>",
                "        )",
                "        {",
                f"            rel material:binding = </MaterialBasics/Looks/{name}>",
                "            float3 xformOp:scale = (0.4, 0.4, 0.4)",
                f"            double3 xformOp:translate = ({number(x)}, {number(y)}, 0)",
                '            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]',
                "        }",
            ]
            if name != names[-1]:
                lines.append("")
    lines += ["    }", "}", ""]
    return "\n".join(lines)


def png(width, height, channels, texel):
    """An 8-bit PNG of `channels` channels (grey, grey-alpha, RGB or RGBA)
    whose texel at column x and row y from the top is texel(x, y).

    The zlib stream holds stored blocks only, so the bytes depend on no
    compressor version; no gamma or colour-space chunk is written.
    """
    rows = b"".join(b"\0" + bytes(c for x in range(width) for c in texel(x, y))
                    for y in range(height))
    blocks = b""
    for start in range(0, len(rows), 65535):
        block = rows[start:start + 65535]
        final = start + 65535 >= len(rows)
        blocks += struct.pack("<BHH", final, len(block), len(block) ^ 0xFFFF) + block
    stream = b"\x78\x01" + blocks + struct.pack(">I", zlib.adler32(rows))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))

    colour_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    header = struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", stream) +
            chunk(b"IEND", b""))


def cutout_alpha(x, y):
    """cutout.png's alpha at column x and row y from the top, by the squared
    distance of the texel's centre from the image's, in half texels."""
    distance = (2 * x - 7) ** 2 + (2 * y - 7) ** 2
    return 255 if distance <= 26 else 96 if distance <= 50 else 0


# material-textures-v1's images in validation/materials/textures/: width,
# height, channels and each texel's channels as a function of its column x
# and row y from the top. The C++ mirror computes the same values.
IMAGES = {
    # Red by column, green by row, blue alternating: every texel differs.
    "palette.png": (4, 4, 3, lambda x, y: (40 + 60 * x, 40 + 60 * y,
                                           200 if (x + y) % 2 == 0 else 60)),
    "ramp.png": (4, 4, 1, lambda x, y: (15 + 15 * (x + 4 * y),)),
    "grey-alpha.png": (4, 4, 2, lambda x, y: (50 + 60 * x, 230 - 60 * y)),
    "rgba.png": (4, 4, 4, lambda x, y: (220 - 60 * x, 40 + 60 * y,
                                        40 + 60 * ((x + y) % 4), 30 + 65 * x)),
    # One-texel cells, minified about 2.4 times on its card.
    "checker.png": (64, 64, 1, lambda x, y: (255 if (x + y) % 2 == 0 else 0,)),
    # material-normals-v1's normal maps, unsigned 8-bit: a dome tilted towards
    # increasing s on the right and increasing t at the top, and the same
    # dome with green running down, as DirectX-convention maps store it.
    "tilts.png": (4, 4, 3, lambda x, y: (32 + 64 * x, 224 - 64 * y, 200)),
    "tilts-dx.png": (4, 4, 3, lambda x, y: (32 + 64 * x, 31 + 64 * y, 200)),
    # material-alpha-v1's cut-out: a green gradient whose alpha is an opaque
    # disc of 24 texels, a ring of 20 at 96 and 20 transparent ones outside.
    "cutout.png": (8, 8, 4, lambda x, y: (60 + 20 * x, 200 - 15 * y, 60, cutout_alpha(x, y))),
}

# Texture coordinates per face vertex of the card quad, whose points run
# counter-clockwise from the bottom left: the image once, or from -0.5 to
# 1.5 so that every wrap mode shows.
UNIT = ((0, 0), (1, 0), (1, 1), (0, 1))
WIDE = ((-0.5, -0.5), (1.5, -0.5), (1.5, 1.5), (-0.5, 1.5))
# Each input's USD type, by node type.
INPUT_TYPES = {
    "UsdPreviewSurface": {"diffuseColor": "color3f", "emissiveColor": "color3f",
                          "ior": "float", "metallic": "float", "normal": "normal3f",
                          "opacity": "float", "opacityMode": "token",
                          "opacityThreshold": "float", "roughness": "float"},
    "UsdUVTexture": {"bias": "float4", "fallback": "float4", "file": "asset",
                     "scale": "float4", "sourceColorSpace": "token", "st": "float2",
                     "wrapS": "token", "wrapT": "token"},
    "UsdPrimvarReader_float2": {"fallback": "float2", "varname": "string"},
    "UsdTransform2d": {"in": "float2", "rotation": "float", "scale": "float2",
                       "translation": "float2"},
}
OUTPUT_TYPES = {"rgb": "float3", "r": "float", "g": "float", "b": "float", "a": "float",
                "result": "float2"}


def output(node, name):
    """A connection to `node`'s output `name`."""
    return ("connect", node, name)


def texture(file, space=None, st=output("Reader", "result"), wrap="clamp", **inputs):
    """A UsdUVTexture node of `file` in validation/materials/textures/, read
    at `st` with `wrap` along both coordinates unless given otherwise."""
    authored = {"st": st, **inputs}
    if file is not None:
        authored["file"] = f"./textures/{file}"
    if space is not None:
        authored["sourceColorSpace"] = space
    if wrap is not None:
        authored.setdefault("wrapS", wrap)
        authored.setdefault("wrapT", wrap)
    return ("UsdUVTexture", authored)


def reader(varname="st", **inputs):
    return ("UsdPrimvarReader_float2", {"varname": varname, **inputs})


def card(surface, **nodes):
    """A material: its UsdPreviewSurface inputs and its other shader nodes."""
    return {"surface": surface, "nodes": nodes}


RGB = output("Texture", "rgb")
# material-textures-v1: the policy's third audit item, textures and UV, on a
# 5x5 grid of cards. Rows from the top: wrap modes; colour spaces and channel
# counts; scalar channels, scale and bias and missing images; texture-
# coordinate sources; then connections the translator does not read, and
# minification. Each entry is the card's name, its material and, where they
# differ from st per face vertex over UNIT, its texture-coordinate primvars
# as (values, interpolation, indices).
TEXTURES = [
    [("UseMetadata", card({"diffuseColor": RGB},
                          Texture=texture("palette.png", "sRGB", wrap=None),
                          Reader=reader()), {"st": (WIDE,)}),
     ("Clamp", card({"diffuseColor": RGB}, Texture=texture("palette.png", "sRGB"),
                    Reader=reader()), {"st": (WIDE,)}),
     ("Repeat", card({"diffuseColor": RGB},
                     Texture=texture("palette.png", "sRGB", wrap="repeat"),
                     Reader=reader()), {"st": (WIDE,)}),
     ("Mirror", card({"diffuseColor": RGB},
                     Texture=texture("palette.png", "sRGB", wrap="mirror"),
                     Reader=reader()), {"st": (WIDE,)}),
     ("Mixed", card({"diffuseColor": RGB},
                    Texture=texture("palette.png", "sRGB", wrapS="repeat", wrapT="black"),
                    Reader=reader()), {"st": (WIDE,)})],
    [("Srgb", card({"diffuseColor": RGB}, Texture=texture("palette.png", "sRGB"),
                   Reader=reader()), {}),
     ("Raw", card({"diffuseColor": RGB}, Texture=texture("palette.png", "raw"),
                  Reader=reader()), {}),
     ("Auto", card({"diffuseColor": RGB}, Texture=texture("palette.png"),
                   Reader=reader()), {}),
     ("Grey", card({"diffuseColor": RGB}, Texture=texture("ramp.png"),
                   Reader=reader()), {}),
     ("GreyAlpha", card({"diffuseColor": RGB, "roughness": output("Texture", "a")},
                        Texture=texture("grey-alpha.png"), Reader=reader()), {})],
    [("Alpha", card({"diffuseColor": RGB, "roughness": output("Texture", "a")},
                    Texture=texture("rgba.png", "sRGB"), Reader=reader()), {}),
     ("Orm", card({"diffuseColor": (0.95, 0.64, 0.54),
                   "metallic": output("Texture", "b"),
                   "roughness": output("Texture", "g")},
                  Texture=texture("rgba.png", "raw"), Reader=reader()), {}),
     ("ScaleBias", card({"diffuseColor": RGB},
                        Texture=texture("palette.png", "raw", scale=(2, 2, 2, 1),
                                        bias=(-0.5, -0.5, -0.5, 0)),
                        Reader=reader()), {}),
     ("Missing", card({"diffuseColor": RGB},
                      Texture=texture("missing.png", "sRGB", scale=(0.5, 0.5, 0.5, 1),
                                      fallback=(0.9, 0.2, 0.6, 1)),
                      Reader=reader()), {}),
     ("NoFile", card({"diffuseColor": RGB},
                     Texture=texture(None, fallback=(0.2, 0.6, 0.9, 1)),
                     Reader=reader()), {})],
    [("Vertex", card({"diffuseColor": RGB}, Texture=texture("palette.png", "sRGB"),
                     Reader=reader()), {"st": (UNIT, "vertex")}),
     ("Indexed", card({"diffuseColor": RGB}, Texture=texture("palette.png", "sRGB"),
                      Reader=reader()),
      {"st": (((0, 0), (1, 0), (0, 1), (1, 1)), "faceVarying", (1, 0, 2, 3))}),
     ("Named", card({"diffuseColor": RGB}, Texture=texture("palette.png", "sRGB"),
                    Reader=reader("uv1")),
      {"st": (UNIT,), "uv1": (((1, 0), (1, 1), (0, 1), (0, 0)),)}),
     ("Constant", card({"diffuseColor": RGB},
                       Texture=texture("palette.png", "sRGB", st=(0.6, 0.3))), {}),
     ("Absent", card({"diffuseColor": RGB}, Texture=texture("palette.png", "sRGB"),
                     Reader=reader("absent", fallback=(0.1, 0.9))), {})],
    [("Transform2d", card({"diffuseColor": RGB},
                          Texture=texture("palette.png", "sRGB",
                                          st=output("Transform", "result")),
                          Transform=("UsdTransform2d",
                                     {"in": output("Reader", "result"), "rotation": 30,
                                      "scale": (2, 2), "translation": (0.25, 0)}),
                          Reader=reader()), {}),
     ("SecondSource", card({"diffuseColor": RGB, "metallic": output("Ramp", "r")},
                           Texture=texture("palette.png", "sRGB"),
                           Ramp=texture("ramp.png", "raw", st=output("Second", "result")),
                           Reader=reader(), Second=reader("uv1")),
      {"st": (UNIT,), "uv1": (((1, 0), (1, 1), (0, 1), (0, 0)),)}),
     ("ColourOnScalar", card({"diffuseColor": (0.3, 0.5, 0.7), "roughness": RGB},
                             Texture=texture("palette.png", "raw"), Reader=reader()), {}),
     ("ScalarOnColour", card({"diffuseColor": output("Texture", "r")},
                             Texture=texture("ramp.png", "raw"), Reader=reader()), {}),
     ("Minified", card({"diffuseColor": RGB},
                       Texture=texture("checker.png", "raw", wrap="repeat"),
                       Reader=reader()), {})],
]
CARD_COLUMNS = (-1.6, -0.8, 0, 0.8, 1.6)
CARD_ROWS = (1.6, 0.8, 0, -0.8, -1.6)


def value(kind, authored):
    if kind == "asset":
        return f"@{authored}@"
    if kind in ("string", "token"):
        return f'"{authored}"'
    if isinstance(authored, tuple):
        return f"({', '.join(number(c) for c in authored)})"
    return number(authored)


def shader(material, name, kind, inputs):
    """A Shader prim's lines, inputs in name order, then the outputs that
    other nodes of `material` read."""
    lines = [f'            def Shader "{name}"', "            {",
             f'                uniform token info:id = "{kind}"']
    for key in sorted(inputs):
        authored = inputs[key]
        prefix = f"{INPUT_TYPES[kind][key]} inputs:{key}"
        if isinstance(authored, tuple) and authored and authored[0] == "connect":
            lines.append(f"                {prefix}.connect = "
                         f"<{material}/{authored[1]}.outputs:{authored[2]}>")
        else:
            lines.append(f"                {prefix} = {value(INPUT_TYPES[kind][key], authored)}")
    return lines


def primvar(name, values, interpolation="faceVarying", indices=None):
    lines = [f"            texCoord2f[] primvars:{name} = [{vectors(values)}] (",
             f'                interpolation = "{interpolation}"', "            )"]
    if indices is not None:
        lines.append(f"            int[] primvars:{name}:indices = [{integers(indices)}]")
    return lines


CARD_CLASS = [
    "# The unit card every card inherits, facing +Z, without normals.",
    'class Mesh "_Card"',
    "{",
    "    int[] faceVertexCounts = [4]",
    "    int[] faceVertexIndices = [0, 1, 2, 3]",
    "    point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]",
    '    uniform token subdivisionScheme = "none"',
    "}",
]


def header(root, mirror, classes):
    """A fixture's layer metadata, the classes its prims inherit and the
    opening of its default prim `root`, holding material-basics-v1's camera,
    lights and backdrop."""
    lines = [
        "#usda 1.0",
        "(",
        f'    defaultPrim = "{root}"',
        "    metersPerUnit = 1",
        '    upAxis = "Y"',
        ")",
        "",
        "# Generated by scripts/material_fixtures.py; edit it there. Mirrored by",
        f"# adapters/common/material_fixtures.cpp ({mirror}).",
        "",
        *classes,
        "",
        f'def Xform "{root}"',
        "{",
    ]
    shared = basics().splitlines()
    start = shared.index('    def Camera "Camera"')
    return lines + shared[start:shared.index('    def Scope "Looks"')]


def looks(root, materials):
    """The Looks scope of `root`'s (name, material) pairs."""
    lines = ['    def Scope "Looks"', "    {"]
    for index, (name, material) in enumerate(materials):
        path = f"/{root}/Looks/{name}"
        used = {}
        for authored in [*material["surface"].values(),
                         *(v for _, inputs in material["nodes"].values()
                           for v in inputs.values())]:
            if isinstance(authored, tuple) and authored and authored[0] == "connect":
                used.setdefault(authored[1], set()).add(authored[2])
        lines += [
            f'        def Material "{name}"',
            "        {",
            f"            token outputs:surface.connect = <{path}/Surface.outputs:surface>",
            "",
        ]
        lines += shader(path, "Surface", "UsdPreviewSurface", material["surface"])
        lines += ["                token outputs:surface", "            }"]
        for node in sorted(material["nodes"]):
            kind, inputs = material["nodes"][node]
            lines += [""] + shader(path, node, kind, inputs)
            lines += [f"                {OUTPUT_TYPES[o]} outputs:{o}"
                      for o in sorted(used.get(node, ()))]
            lines.append("            }")
        lines.append("        }")
        if index + 1 < len(materials):
            lines.append("")
    return lines + ["    }"]


def textures():
    lines = header("MaterialTextures", "SetMaterialTextures", CARD_CLASS)
    lines += looks("MaterialTextures",
                   [(name, material) for row in TEXTURES for name, material, _ in row])
    names = [name for row in TEXTURES for name, _, _ in row]
    lines += ["", '    def Scope "Cards"', "    {"]
    for y, row in zip(CARD_ROWS, TEXTURES):
        for x, (name, _, primvars) in zip(CARD_COLUMNS, row):
            lines += [
                f'        def Mesh "{name}" (',
                '            prepend apiSchemas = ["MaterialBindingAPI"]',
                "            prepend inherits = </_Card>",
                "        )",
                "        {",
                f"            rel material:binding = </MaterialTextures/Looks/{name}>",
            ]
            for set_name, layout in sorted((primvars or {"st": (UNIT,)}).items()):
                lines += primvar(set_name, *layout)
            lines += [
                "            float3 xformOp:scale = (0.7, 0.7, 0.7)",
                f"            double3 xformOp:translate = ({number(x)}, {number(y)}, 0)",
                '            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]',
                "        }",
            ]
            if name != names[-1]:
                lines.append("")
    lines += ["    }", "}", ""]
    return "\n".join(lines)


# material-normals-v1's sphere: coarse, so that faceting shows.
COARSE_SEGMENTS = 16
COARSE_RINGS = 8


def latlong(scale):
    """The coarse sphere's (s, t) per face vertex, in face order: s from 0 to
    1 with the azimuth, t from 1 at the top pole to 0, both times `scale`.
    A pole corner takes its triangle's middle s; the seam repeats s = 0 as 1.
    """
    segments, rings = COARSE_SEGMENTS, COARSE_RINGS

    def st(s, t):
        return (s * scale[0], t * scale[1])

    values = []
    for j in range(segments):
        values += [st((j + 0.5) / segments, 1), st(j / segments, 1 - 1 / rings),
                   st((j + 1) / segments, 1 - 1 / rings)]
    for i in range(1, rings - 1):
        for j in range(segments):
            top, bottom = 1 - i / rings, 1 - (i + 1) / rings
            values += [st(j / segments, top), st(j / segments, bottom),
                       st((j + 1) / segments, bottom), st((j + 1) / segments, top)]
    for j in range(segments):
        values += [st(j / segments, 1 / rings), st((j + 0.5) / segments, 0),
                   st((j + 1) / segments, 1 / rings)]
    return values


def face_normals():
    """The coarse sphere's unit face-centroid directions, one per face."""
    points, counts, indices = sphere(COARSE_SEGMENTS, COARSE_RINGS)
    normals, first = [], 0
    for count in counts:
        centre = [sum(points[i][k] for i in indices[first:first + count]) for k in range(3)]
        first += count
        length = math.sqrt(sum(c * c for c in centre))
        normals.append(tuple(c / length for c in centre))
    return normals


def sphere_class():
    """The class of the coarse unit sphere, without normals."""
    points, counts, indices = sphere(COARSE_SEGMENTS, COARSE_RINGS)
    return [
        "# The coarse unit sphere the spheres inherit, without normals.",
        'class Mesh "_Sphere"',
        "{",
        f"    int[] faceVertexCounts = [{integers(counts)}]",
        f"    int[] faceVertexIndices = [{integers(indices)}]",
        f"    point3f[] points = [{vectors(points)}]",
        '    uniform token subdivisionScheme = "none"',
        "}",
        "",
    ]


def normal_map(file, space="raw", wrap="clamp", st=output("Reader", "result"), **inputs):
    """A UsdUVTexture node reading an unsigned normal map, decoded to signed
    by scale 2 and bias -1 unless given otherwise."""
    inputs.setdefault("scale", (2, 2, 2, 1))
    inputs.setdefault("bias", (-1, -1, -1, 0))
    return texture(file, space, st=st, wrap=wrap, **inputs)


GREY = (0.7, 0.7, 0.7)
COPPER = (0.95, 0.64, 0.54)
NORMAL = output("Texture", "rgb")
# A tangent-space constant, as authored; normalized in the shader.
TILTED = (0.4, 0.3, 0.866)
# material-normals-v1's materials. Smooth shades the mesh-normal rows; the
# others perturb the mesh normal, by a map or a constant.
NORMAL_LOOKS = [
    ("Smooth", card({"diffuseColor": (0.6, 0.6, 0.6), "roughness": 0.3})),
    ("Mapped", card({"diffuseColor": GREY, "roughness": 0.4, "normal": NORMAL},
                    Texture=normal_map("tilts.png"), Reader=reader())),
    ("Auto", card({"diffuseColor": GREY, "roughness": 0.4, "normal": NORMAL},
                  Texture=normal_map("tilts.png", None), Reader=reader())),
    ("Unscaled", card({"diffuseColor": GREY, "roughness": 0.4, "normal": NORMAL},
                      Texture=texture("tilts.png", "raw"), Reader=reader())),
    ("DirectX", card({"diffuseColor": GREY, "roughness": 0.4, "normal": NORMAL},
                     Texture=normal_map("tilts-dx.png", scale=(2, -2, 2, 1),
                                        bias=(-1, 1, -1, 0)),
                     Reader=reader())),
    ("Constant", card({"diffuseColor": GREY, "roughness": 0.4, "normal": TILTED})),
    ("AbsentSet", card({"diffuseColor": GREY, "roughness": 0.4, "normal": NORMAL},
                       Texture=normal_map("tilts.png"),
                       Reader=reader("absent", fallback=(0.25, 0.75)))),
    ("SphereMapped", card({"diffuseColor": GREY, "roughness": 0.4, "normal": NORMAL},
                          Texture=normal_map("tilts.png", wrap="repeat"), Reader=reader())),
    ("SphereGlossy", card({"diffuseColor": COPPER, "metallic": 1, "roughness": 0.25,
                           "normal": NORMAL},
                          Texture=normal_map("tilts.png", wrap="repeat"), Reader=reader())),
]


def normal_attribute(name, values, interpolation=None, indices=None):
    lines = [f"            normal3f[] {name} = [{vectors(values)}]"]
    if interpolation is not None:
        lines[0] += " ("
        lines += [f'                interpolation = "{interpolation}"', "            )"]
    if indices is not None:
        lines.append(f"            int[] {name}:indices = [{integers(indices)}]")
    return lines


def normals():
    points, counts, indices = sphere(COARSE_SEGMENTS, COARSE_RINGS)
    # Each face's vertices in reverse: clockwise seen from outside.
    reversed_indices, first = [], 0
    for count in counts:
        reversed_indices += indices[first:first + count][::-1]
        first += count
    catmull_clark = ['            uniform token subdivisionScheme = "catmullClark"']
    vertex = normal_attribute("normals", points)
    mapped_st = primvar("st", latlong((4, 2)))
    # Rows from the top: mesh-normal interpolations; fallbacks for meshes
    # without usable normals, and inward normals; normal-map encodings and a
    # constant; tangent frames; normal maps on spheres. Each entry is the
    # object's name, the class it inherits, its material, the lines it adds
    # and its scale.
    sphere_scale, card_scale = (0.35, 0.35, 0.35), (0.7, 0.7, 0.7)
    unit_st = primvar("st", UNIT)
    rows = [
        [("Faceted", "_Sphere", "Smooth", [], sphere_scale),
         ("Vertex", "_Sphere", "Smooth", vertex, sphere_scale),
         ("FaceVarying", "_Sphere", "Smooth",
          normal_attribute("primvars:normals", points, "faceVarying", indices), sphere_scale),
         ("Uniform", "_Sphere", "Smooth",
          normal_attribute("normals", face_normals(), "uniform"), sphere_scale),
         ("Primvar", "_Sphere", "Smooth",
          normal_attribute("normals", [(0, 0, 1)] * len(points)) +
          normal_attribute("primvars:normals", points, "vertex"), sphere_scale)],
        [("Computed", "_Sphere", "Smooth", catmull_clark, sphere_scale),
         ("LeftHanded", "_Sphere", "Smooth",
          [f"            int[] faceVertexIndices = [{integers(reversed_indices)}]",
           '            uniform token orientation = "leftHanded"', *catmull_clark],
          sphere_scale),
         ("Malformed", "_Sphere", "Smooth",
          normal_attribute("normals", points[:3]) + catmull_clark, sphere_scale),
         ("Bilinear", "_Sphere", "Smooth",
          ['            uniform token subdivisionScheme = "bilinear"'], sphere_scale),
         ("Inward", "_Sphere", "Smooth",
          normal_attribute("normals", [tuple(-c for c in p) for p in points]), sphere_scale)],
        [("Mapped", "_Card", "Mapped", unit_st, card_scale),
         ("Auto", "_Card", "Auto", unit_st, card_scale),
         ("Unscaled", "_Card", "Unscaled", unit_st, card_scale),
         ("DirectX", "_Card", "DirectX", unit_st, card_scale),
         ("Constant", "_Card", "Constant", unit_st, card_scale)],
        [("MirroredS", "_Card", "Mapped",
          primvar("st", ((1, 0), (0, 0), (0, 1), (1, 1))), card_scale),
         ("Rotated", "_Card", "Mapped",
          primvar("st", ((1, 0), (1, 1), (0, 1), (0, 0))), card_scale),
         ("MirroredTransform", "_Card", "Mapped", unit_st, (-0.7, 0.7, 0.7)),
         ("Stretched", "_Card", "Mapped", unit_st, (0.7, 0.35, 0.7)),
         ("AbsentSet", "_Card", "AbsentSet", unit_st, card_scale)],
        [("SphereVertex", "_Sphere", "SphereMapped", vertex + mapped_st, sphere_scale),
         ("SphereFaceted", "_Sphere", "SphereMapped", mapped_st, sphere_scale),
         ("SphereComputed", "_Sphere", "SphereMapped", catmull_clark + mapped_st,
          sphere_scale),
         ("SphereGlossy", "_Sphere", "SphereGlossy", vertex + mapped_st, sphere_scale),
         ("SphereConstant", "_Sphere", "Constant", vertex + mapped_st, sphere_scale)],
    ]
    lines = header("MaterialNormals", "SetMaterialNormals", sphere_class() + CARD_CLASS)
    lines += looks("MaterialNormals", NORMAL_LOOKS)
    lines += ["", '    def Scope "Objects"', "    {"]
    for y, row in zip(CARD_ROWS, rows):
        for x, (name, base, material, extra, scale) in zip(CARD_COLUMNS, row):
            lines += [
                f'        def Mesh "{name}" (',
                '            prepend apiSchemas = ["MaterialBindingAPI"]',
                f"            prepend inherits = </{base}>",
                "        )",
                "        {",
                f"            rel material:binding = </MaterialNormals/Looks/{material}>",
                *extra,
                f"            float3 xformOp:scale = ({', '.join(number(c) for c in scale)})",
                f"            double3 xformOp:translate = ({number(x)}, {number(y)}, 0)",
                '            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]',
                "        }",
            ]
            if (x, y) != (CARD_COLUMNS[-1], CARD_ROWS[-1]):
                lines.append("")
    lines += ["    }", "}", ""]
    return "\n".join(lines)


def radiance(width, height, texel):
    """A Radiance RGBE image whose texel at column x and row y from the top
    has the bytes texel(x, y): three mantissas and a shared exponent, so it
    decodes to mantissa * 2^(exponent - 136) exactly. Scanlines are flat, as
    they must be below a width of 8."""
    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode()
    return header + bytes(c for y in range(height) for x in range(width) for c in texel(x, y))


# material-emission-v1's HDR image: red falling and green rising with the
# column, constant blue, doubling down each row, from at most 0.47 on the
# top row to 3.75 on the bottom one.
GLOW = (4, 4, lambda x, y: (240 - 50 * x, 80 + 50 * x, 160, 127 + y))

WARM = (1, 0.6, 0.2)
# material-emission-v1's materials. Unless stated, base colour and the other
# inputs take their defaults; textured emission reads the palette under
# clamp at st.
EMISSION_LOOKS = [
    ("Warm", card({"emissiveColor": WARM})),
    ("Bright", card({"emissiveColor": (6, 6, 6)})),
    ("Dim", card({"emissiveColor": (0.05, 0.1, 0.2)})),
    ("Clamped", card({"emissiveColor": (2, -1, 0.5)})),
    ("NonFinite", card({"emissiveColor": (math.inf, 1, 1)})),
    ("Metal", card({"diffuseColor": COPPER, "metallic": 1, "roughness": 0.25,
                    "emissiveColor": (0.6, 0.2, 0.1)})),
    ("Cool", card({"emissiveColor": (0.4, 0.8, 1)})),
    ("Srgb", card({"emissiveColor": RGB}, Texture=texture("palette.png", "sRGB"),
                  Reader=reader())),
    ("Raw", card({"emissiveColor": RGB}, Texture=texture("palette.png", "raw"),
                 Reader=reader())),
    ("Auto", card({"emissiveColor": RGB}, Texture=texture("palette.png"), Reader=reader())),
    ("Grey", card({"emissiveColor": RGB}, Texture=texture("ramp.png"), Reader=reader())),
    ("ScaleBias", card({"emissiveColor": RGB},
                       Texture=texture("palette.png", "raw", scale=(3, 3, 3, 1),
                                       bias=(0.2, 0.1, 0, 0)),
                       Reader=reader())),
    ("Missing", card({"emissiveColor": RGB},
                     Texture=texture("missing.png", "sRGB", fallback=(2, 1, 0.5, 1)),
                     Reader=reader())),
    ("Inverted", card({"emissiveColor": RGB},
                      Texture=texture("palette.png", "raw", scale=(-1, -1, -1, 1),
                                      bias=(1, 1, 1, 0)),
                      Reader=reader())),
    ("Hdr", card({"emissiveColor": RGB}, Texture=texture("glow.hdr", "raw"), Reader=reader())),
    ("HdrAuto", card({"emissiveColor": RGB}, Texture=texture("glow.hdr"), Reader=reader())),
    ("HdrSrgb", card({"emissiveColor": RGB}, Texture=texture("glow.hdr", "sRGB"),
                     Reader=reader())),
    ("SphereTextured", card({"emissiveColor": RGB},
                            Texture=texture("palette.png", "sRGB", wrap="repeat"),
                            Reader=reader())),
    ("Shared", card({"diffuseColor": RGB, "emissiveColor": RGB},
                    Texture=texture("palette.png", "sRGB"), Reader=reader())),
    # An unlit asset: no diffuse, and ior 1 removes the specular interface.
    ("Unlit", card({"diffuseColor": (0, 0, 0), "ior": 1, "emissiveColor": RGB},
                   Texture=texture("palette.png", "sRGB"), Reader=reader())),
    ("Base", card({"diffuseColor": (0.6, 0.6, 0.6), "roughness": 0.3})),
]


def quoted(token):
    return f'"{token}"'


def emission():
    unit_st = primvar("st", UNIT)
    sphere_scale, card_scale = (0.35, 0.35, 0.35), (0.7, 0.7, 0.7)
    # A card of two quads, left and right, whose right face a GeomSubset
    # binds to an emissive material over the mesh's grey.
    split = [
        "            int[] faceVertexCounts = [4, 4]",
        "            int[] faceVertexIndices = [0, 1, 4, 5, 1, 2, 3, 4]",
        "            point3f[] points = [(-0.5, -0.5, 0), (0, -0.5, 0), (0.5, -0.5, 0), "
        "(0.5, 0.5, 0), (0, 0.5, 0), (-0.5, 0.5, 0)]",
        '            uniform token subsetFamily:materialBind:familyType = "nonOverlapping"',
    ]
    panel = [
        "",
        '            def GeomSubset "Panel" (',
        '                prepend apiSchemas = ["MaterialBindingAPI"]',
        "            )",
        "            {",
        '                uniform token elementType = "face"',
        '                uniform token familyName = "materialBind"',
        "                int[] indices = [1]",
        "                rel material:binding = </MaterialEmission/Looks/Warm>",
        "            }",
    ]
    # Rows from the top: constant emission; emitters' surfaces, sides and
    # transforms; 8-bit emission maps; fallbacks and HDR maps; then a tilted
    # emitter, a textured sphere and arrangements real assets use. Each entry
    # is the object's name, the class it inherits, its material, the lines it
    # adds before its transform, its scale, its rotation about X in degrees
    # and the lines it adds after.
    rows = [
        [("Warm", "_Card", "Warm", [], card_scale, None, []),
         ("Bright", "_Card", "Bright", [], card_scale, None, []),
         ("Dim", "_Card", "Dim", [], card_scale, None, []),
         ("Clamped", "_Card", "Clamped", [], card_scale, None, []),
         ("NonFinite", "_Card", "NonFinite", [], card_scale, None, [])],
        [("Metal", "_Sphere", "Metal", normal_attribute(
            "normals", sphere(COARSE_SEGMENTS, COARSE_RINGS)[0]), sphere_scale, None, []),
         ("Back", "_Card", "Warm", ["            int[] faceVertexIndices = [0, 3, 2, 1]"],
          card_scale, None, []),
         ("Mirrored", "_Card", "Warm", [], (-0.7, 0.7, 0.7), None, []),
         ("Stretched", "_Card", "Warm", [], (0.7, 0.35, 0.7), None, []),
         ("Sphere", "_Sphere", "Cool", [], sphere_scale, None, [])],
        [(name, "_Card", name, unit_st, card_scale, None, [])
         for name in ("Srgb", "Raw", "Auto", "Grey", "ScaleBias")],
        [(name, "_Card", name, unit_st, card_scale, None, [])
         for name in ("Missing", "Inverted", "Hdr", "HdrAuto", "HdrSrgb")],
        [("Tilted", "_Card", "Warm", [], card_scale, 60, []),
         ("TexturedSphere", "_Sphere", "SphereTextured", primvar("st", latlong((4, 2))),
          sphere_scale, None, []),
         ("Shared", "_Card", "Shared", unit_st, card_scale, None, []),
         ("Unlit", "_Card", "Unlit", unit_st, card_scale, None, []),
         ("Subset", "_Card", "Base", split, card_scale, None, panel)],
    ]
    lines = header("MaterialEmission", "SetMaterialEmission", sphere_class() + CARD_CLASS)
    lines += looks("MaterialEmission", EMISSION_LOOKS)
    lines += ["", '    def Scope "Objects"', "    {"]
    for y, row in zip(CARD_ROWS, rows):
        for x, (name, base, material, extra, scale, rotate_x, after) in zip(CARD_COLUMNS, row):
            ops = ["xformOp:translate", "xformOp:scale"]
            rotation = []
            if rotate_x is not None:
                ops.insert(1, "xformOp:rotateX")
                rotation = [f"            float xformOp:rotateX = {number(rotate_x)}"]
            lines += [
                f'        def Mesh "{name}" (',
                '            prepend apiSchemas = ["MaterialBindingAPI"]',
                f"            prepend inherits = </{base}>",
                "        )",
                "        {",
                f"            rel material:binding = </MaterialEmission/Looks/{material}>",
                *extra,
                *rotation,
                f"            float3 xformOp:scale = ({', '.join(number(c) for c in scale)})",
                f"            double3 xformOp:translate = ({number(x)}, {number(y)}, 0)",
                f"            uniform token[] xformOpOrder = [{', '.join(map(quoted, ops))}]",
                *after,
                "        }",
            ]
            if (x, y) != (CARD_COLUMNS[-1], CARD_ROWS[-1]):
                lines.append("")
    lines += ["    }", "}", ""]
    return "\n".join(lines)


TEAL = (0.2, 0.6, 0.7)
ALPHA = output("Texture", "a")


def masked(file, space="sRGB", threshold=0.5, **inputs):
    """A material whose base colour and coverage are an image's RGB and
    alpha, read at st under `threshold`."""
    surface = {"diffuseColor": RGB, "opacity": ALPHA}
    if threshold:
        surface["opacityThreshold"] = threshold
    return card(surface, Texture=texture(file, space, **inputs), Reader=reader())


# material-alpha-v1's materials. Constant cards are teal; masks threshold at
# 0.5 unless stated.
ALPHA_LOOKS = [
    ("Half", card({"diffuseColor": TEAL, "opacity": 0.5})),
    ("Quarter", card({"diffuseColor": TEAL, "opacity": 0.25})),
    ("Zero", card({"diffuseColor": TEAL, "opacity": 0})),
    ("Presence", card({"diffuseColor": TEAL, "opacity": 0.5, "opacityMode": "presence"})),
    ("Clamped", card({"diffuseColor": TEAL, "opacity": 1.5, "opacityThreshold": 2})),
    ("Below", card({"diffuseColor": TEAL, "opacity": 0.4, "opacityThreshold": 0.5})),
    ("Equal", card({"diffuseColor": TEAL, "opacity": 0.5, "opacityThreshold": 0.5})),
    ("Above", card({"diffuseColor": TEAL, "opacity": 0.6, "opacityThreshold": 0.5})),
    ("NonFinite", card({"diffuseColor": TEAL, "opacity": -math.inf,
                        "opacityThreshold": math.inf})),
    # The specification makes the threshold interface-only: not a texture input.
    ("ThresholdMap", card({"diffuseColor": TEAL, "opacity": 0.5,
                           "opacityThreshold": output("Ramp", "r")},
                          Ramp=texture("ramp.png", "raw"), Reader=reader())),
    ("Alpha", masked("rgba.png")),
    ("Cutout", masked("cutout.png")),
    ("GreyAlpha", masked("grey-alpha.png", None)),
    ("Ramp", card({"diffuseColor": TEAL, "opacity": output("Texture", "r"),
                   "opacityThreshold": 0.5},
                  Texture=texture("ramp.png", "raw"), Reader=reader())),
    ("NoAlpha", masked("palette.png")),
    ("Fractional", masked("cutout.png", threshold=0)),
    ("Inverted", card({"diffuseColor": TEAL, "opacity": ALPHA, "opacityThreshold": 0.5},
                      Texture=texture("cutout.png", "sRGB", scale=(1, 1, 1, -1),
                                      bias=(0, 0, 0, 1)),
                      Reader=reader())),
    ("BlackBorder", masked("palette.png", wrap="black")),
    ("Missing", masked("missing.png", threshold=0, fallback=(0.9, 0.2, 0.6, 0.3))),
    ("Minified", card({"diffuseColor": TEAL, "opacity": output("Texture", "r"),
                       "opacityThreshold": 0.5},
                      Texture=texture("checker.png", "raw", wrap="repeat"), Reader=reader())),
    ("SphereCutout", masked("cutout.png", wrap="repeat")),
    ("GlowingMask", card({"emissiveColor": WARM, "opacity": ALPHA, "opacityThreshold": 0.5},
                         Texture=texture("cutout.png", "sRGB"), Reader=reader())),
    ("GlowingHalf", card({"emissiveColor": WARM, "opacity": 0.5})),
]


def alpha():
    unit_st = primvar("st", UNIT)
    sphere_scale, card_scale = (0.35, 0.35, 0.35), (0.7, 0.7, 0.7)
    # Two quads, the back one the unit card and a smaller one 0.4 in front of
    # it, overlapping its upper right.
    stacked = [
        "            int[] faceVertexCounts = [4, 4]",
        "            int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7]",
        "            point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), "
        "(-0.5, 0.5, 0), (-0.15, -0.15, 0.4), (0.45, -0.15, 0.4), (0.45, 0.45, 0.4), "
        "(-0.15, 0.45, 0.4)]",
    ]
    # Unbound, as geometry without a material is authored for Storm.
    display = [
        "            color3f[] primvars:displayColor = [(0.2, 0.6, 0.7)]",
        "            float[] primvars:displayOpacity = [0.3]",
    ]
    points = sphere(COARSE_SEGMENTS, COARSE_RINGS)[0]
    # Rows from the top: constant coverage; constant thresholds; masks by
    # channel and image; lookups' coverage, inversion, borders, fallbacks and
    # minification; then arrangements real assets use. Each entry is the
    # object's name, the class it inherits, its material (None for unbound),
    # the lines it adds and its scale.
    rows = [
        [(name, "_Card", name, [], card_scale)
         for name in ("Half", "Quarter", "Zero", "Presence", "Clamped")],
        [(name, "_Card", name, unit_st if name == "ThresholdMap" else [], card_scale)
         for name in ("Below", "Equal", "Above", "NonFinite", "ThresholdMap")],
        [(name, "_Card", name, unit_st, card_scale)
         for name in ("Alpha", "Cutout", "GreyAlpha", "Ramp", "NoAlpha")],
        [("Fractional", "_Card", "Fractional", unit_st, card_scale),
         ("Inverted", "_Card", "Inverted", unit_st, card_scale),
         ("BlackBorder", "_Card", "BlackBorder", primvar("st", WIDE), card_scale),
         ("Missing", "_Card", "Missing", unit_st, card_scale),
         ("Minified", "_Card", "Minified", unit_st, card_scale)],
        [("Stacked", "_Card", "Half", stacked, card_scale),
         ("SphereCutout", "_Sphere", "SphereCutout",
          normal_attribute("normals", points) + primvar("st", latlong((4, 2))), sphere_scale),
         ("GlowingMask", "_Card", "GlowingMask", unit_st, card_scale),
         ("GlowingHalf", "_Card", "GlowingHalf", [], card_scale),
         ("DisplayOpacity", "_Card", None, display, card_scale)],
    ]
    lines = header("MaterialAlpha", "SetMaterialAlpha", sphere_class() + CARD_CLASS)
    lines += looks("MaterialAlpha", ALPHA_LOOKS)
    lines += ["", '    def Scope "Objects"', "    {"]
    for y, row in zip(CARD_ROWS, rows):
        for x, (name, base, material, extra, scale) in zip(CARD_COLUMNS, row):
            if material is None:
                lines += [f'        def Mesh "{name}" (', f"            prepend inherits = </{base}>",
                          "        )", "        {"]
            else:
                lines += [
                    f'        def Mesh "{name}" (',
                    '            prepend apiSchemas = ["MaterialBindingAPI"]',
                    f"            prepend inherits = </{base}>",
                    "        )",
                    "        {",
                    f"            rel material:binding = </MaterialAlpha/Looks/{material}>",
                ]
            lines += [
                *extra,
                f"            float3 xformOp:scale = ({', '.join(number(c) for c in scale)})",
                f"            double3 xformOp:translate = ({number(x)}, {number(y)}, 0)",
                '            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]',
                "        }",
            ]
            if (x, y) != (CARD_COLUMNS[-1], CARD_ROWS[-1]):
                lines.append("")
    lines += ["    }", "}", ""]
    return "\n".join(lines)


# Every committed file, relative to DIRECTORY: USD text or image bytes.
FIXTURES = {
    "material-basics-v1.usda": basics,
    "material-textures-v1.usda": textures,
    "material-normals-v1.usda": normals,
    "material-emission-v1.usda": emission,
    "material-alpha-v1.usda": alpha,
    **{f"textures/{name}": (lambda image=image: png(*image)) for name, image in IMAGES.items()},
    "textures/glow.hdr": lambda: radiance(*GLOW),
}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("write", "check"))
    args = parser.parse_args(argv)
    stale = []
    for name, generate in FIXTURES.items():
        path = DIRECTORY / name
        content = generate()
        if args.command == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, str):
                path.write_text(content, encoding="utf-8", newline="\n")
            else:
                path.write_bytes(content)
        elif not path.exists() or (path.read_text(encoding="utf-8")
                                   if isinstance(content, str)
                                   else path.read_bytes()) != content:
            stale.append(name)
    for name in stale:
        print(f"{name} differs from scripts/material_fixtures.py", file=sys.stderr)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
