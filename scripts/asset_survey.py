#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Survey USD assets for features Lotus does not translate (LOTUS-ASSET-01).

`inventory` counts, per stage, the material, geometry and light features
that decide what Lotus can render; it needs the OpenUSD Python modules.
`warnings` classifies the Lotus warnings in `usdrecord` logs; it needs no
OpenUSD. Stage paths are recorded relative to `--root`, so the output names
no machine-local path.
"""

import argparse
import collections
import json
from pathlib import Path
import re
import sys

LOTUS_LIGHTS = {"SphereLight", "RectLight", "DiskLight", "DistantLight", "DomeLight"}
# UsdPreviewSurface inputs the translator reads; the rest are listed when
# authored with a value that changes the surface.
PREVIEW_IGNORED = ("clearcoat", "displacement", "opacityMode")
SURFACE_NODES = ("standard_surface", "open_pbr_surface", "gltf_pbr", "UsdPreviewSurface",
                 "PxrSurface")
# Lobe weights whose nonzero value or texture means the lobe is in use.
LOBES = {
    "standard_surface": ("metalness", "transmission", "subsurface", "sheen", "coat",
                         "thin_film_thickness", "emission", "specular_anisotropy",
                         "diffuse_roughness"),
    "open_pbr_surface": ("base_metalness", "transmission_weight", "subsurface_weight",
                         "fuzz_weight", "coat_weight", "thin_film_weight",
                         "emission_luminance", "specular_roughness_anisotropy",
                         "base_diffuse_roughness"),
    "UsdPreviewSurface": ("metallic", "clearcoat", "displacement"),
}
LIGHT_ATTRIBUTES = ("inputs:shaping:", "inputs:shadow:", "inputs:texture:file",
                    "inputs:enableColorTemperature", "light:filters", "treatAsPoint",
                    "treatAsLine")

WARNINGS = [
    ("no surface terminal", re.compile(r"default material for .*: the material has no surface terminal")),
    ("unsupported surface", re.compile(r"default material for ")),
    ("untranslated connection", re.compile(r"uses the defaults of .* inputs whose shader graph")),
    ("GeomSubset bindings ignored", re.compile(r"ignores the GeomSubset material bindings")),
    ("texture unreadable", re.compile(r"cannot read the texture")),
    ("mesh rejected", re.compile(r"Lotus rejected mesh .*: (.*)")),
    ("displayColor ignored", re.compile(r"ignores the displayColor of .*: (.*)")),
    ("dome ignored", re.compile(r"Lotus (?:ignores|reads only|cannot read the dome).*dome", re.I)),
    ("light ignored", re.compile(r"Lotus ignores a light")),
]


def relative_name(path, root):
    path, root = Path(path).resolve(), Path(root).resolve()
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.name


def as_floats(value):
    if value is None:
        return []
    try:
        return [float(v) for v in value] if hasattr(value, "__len__") else [float(value)]
    except (TypeError, ValueError):
        return []


def classify_log(text):
    """Count Lotus warnings by category; other warnings by their source."""
    counts = collections.Counter()
    details = collections.Counter()
    for line in text.splitlines():
        if not line.startswith(("Warning", "Coding Error", "Runtime Error", "Error")):
            continue
        message = line.split(" -- ", 1)[-1]
        for name, pattern in WARNINGS:
            match = pattern.search(message)
            if not match:
                continue
            counts[name] += 1
            if name == "untranslated connection":
                for item in message.rsplit(": ", 1)[-1].split(", "):
                    details[f"input {item.strip()}"] += 1
            elif match.groups():
                details[f"{name}: {match[1].strip()}"] += 1
            break
        else:
            counts["other Lotus" if "Lotus" in message else "not Lotus"] += 1
    return {"warnings": dict(counts), "details": dict(details)}


def inventory(path, root):
    from pxr import Sdf, Usd, UsdGeom, UsdLux, UsdShade, UsdSkel

    def shader_id(shader):
        sid = shader.GetIdAttr().Get()
        if sid:
            return str(sid)
        for context in ("mtlx", "glslfx", "ri", "sdr"):
            source = shader.GetSourceAsset(context)
            if source:
                return f"{context}:{source.path}"
        return "<unknown>"

    def upstream(inp):
        sources = inp.GetConnectedSources()[0]
        if not sources:
            return None, None
        prim = sources[0].source.GetPrim()
        if not prim.IsA(UsdShade.Shader):
            return prim, "<node graph>"
        return prim, shader_id(UsdShade.Shader(prim))

    def surface_source(material, context):
        output = material.GetSurfaceOutput(context) if context else material.GetSurfaceOutput()
        if not output or not output.HasConnectedSource():
            return None
        return upstream(output)[1]

    def resolved(inp):
        """("texture", None) when a shader output drives the input, else ("value", v)."""
        attrs = inp.GetValueProducingAttributes()
        if attrs and UsdShade.Output.IsOutput(attrs[0]):
            return "texture", None
        return "value", (attrs[0].Get() if attrs else inp.Get())

    def lobes(shader, sid):
        used = []
        for family, weights in LOBES.items():
            if family not in sid:
                continue
            for name in weights:
                inp = shader.GetInput(name)
                if inp:
                    kind, value = resolved(inp)
                    if kind == "texture":
                        used.append(f"{name} (texture)")
                    elif any(v != 0.0 for v in as_floats(value)):
                        used.append(name)
            opacity = shader.GetInput("opacity")
            if opacity:
                kind, value = resolved(opacity)
                if kind == "texture":
                    used.append("opacity (texture)")
                elif any(v < 1.0 for v in as_floats(value)):
                    used.append("opacity < 1")
            thin = shader.GetInput("thin_walled")
            if thin and resolved(thin)[1]:
                used.append("thin_walled")
        return used

    def preview_graphs(shader):
        """Each connected input's graph shape, other than texture-to-input."""
        shapes = []
        for inp in shader.GetInputs():
            if not inp.HasConnectedSource():
                continue
            prim, source = upstream(inp)
            if source == "UsdUVTexture":
                st = UsdShade.Shader(prim).GetInput("st")
                st_source = upstream(st)[1] if st and st.HasConnectedSource() else None
                if st_source in (None, "UsdPrimvarReader_float2"):
                    continue
                source = f"UsdUVTexture <- {st_source}"
            elif source == "<node graph>":
                inner = sorted({shader_id(UsdShade.Shader(child)) for child in prim.GetChildren()
                                if child.IsA(UsdShade.Shader)})
                source = "node graph of " + ", ".join(inner)
            shapes.append(f"{inp.GetBaseName()}: {source}")
        return shapes

    stage = Usd.Stage.Open(str(path), Usd.Stage.LoadAll)
    gprims = collections.Counter()
    subdivision = collections.Counter()
    surfaces = collections.Counter()
    bindings = collections.Counter()
    lobe_use = collections.defaultdict(collections.Counter)
    graphs = collections.Counter()
    ignored = collections.Counter()
    lights = collections.Counter()
    light_attributes = collections.Counter()
    textures = set()
    counts = collections.Counter()
    kinds = {}

    predicate = Usd.TraverseInstanceProxies(Usd.PrimDefaultPredicate)
    for prim in Usd.PrimRange.Stage(stage, predicate):
        if prim.HasAPI(UsdLux.MeshLightAPI):
            lights["MeshLightAPI"] += 1
        if prim.IsA(UsdGeom.Gprim):
            gprims[str(prim.GetTypeName())] += 1
            if UsdGeom.Imageable(prim).ComputeVisibility() == UsdGeom.Tokens.invisible:
                continue
            if prim.IsA(UsdGeom.Mesh):
                mesh = UsdGeom.Mesh(prim)
                subdivision[str(mesh.GetSubdivisionSchemeAttr().Get())] += 1
                if prim.HasAPI(UsdSkel.BindingAPI) and UsdSkel.BindingAPI(prim).GetSkeleton():
                    counts["skinned meshes"] += 1
            material = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial(
                UsdShade.Tokens.preview)[0]
            if material:
                key = str(material.GetPath())
                if key not in kinds:
                    universal = surface_source(material, None)
                    mtlx = surface_source(material, "mtlx")
                    ri = surface_source(material, "ri")
                    if universal == "UsdPreviewSurface":
                        kinds[key] = "UsdPreviewSurface" + (" + MaterialX" if mtlx else "")
                    elif universal:
                        kinds[key] = f"universal {universal}"
                    elif mtlx:
                        kinds[key] = f"MaterialX only ({mtlx})"
                    elif ri:
                        kinds[key] = f"RenderMan only ({ri})"
                    else:
                        kinds[key] = "no surface"
                bindings[kinds[key]] += 1
                continue
            primvars = UsdGeom.PrimvarsAPI(prim)
            color = primvars.GetPrimvar("displayColor")
            if color and color.HasAuthoredValue():
                if color.GetInterpolation() != UsdGeom.Tokens.constant:
                    counts["unbound non-constant displayColor"] += 1
                else:
                    counts["unbound displayColor"] += 1
            opacity = primvars.GetPrimvar("displayOpacity")
            if opacity and any(v < 1.0 for v in as_floats(opacity.ComputeFlattened())):
                counts["unbound displayOpacity < 1"] += 1
        elif prim.IsA(UsdGeom.Subset):
            if UsdGeom.Subset(prim).GetFamilyNameAttr().Get() == "materialBind" and \
                    UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial(
                        UsdShade.Tokens.preview)[0]:
                counts["GeomSubset material bindings"] += 1
        elif prim.IsA(UsdShade.Shader):
            shader = UsdShade.Shader(prim)
            sid = shader_id(shader)
            for inp in shader.GetInputs():
                value = inp.Get()
                if isinstance(value, Sdf.AssetPath) and value.path:
                    textures.add(value.path)
            if not any(node in sid for node in SURFACE_NODES):
                continue
            surfaces[sid] += 1
            for lobe in lobes(shader, sid):
                lobe_use[sid][lobe] += 1
            if sid == "UsdPreviewSurface":
                graphs.update(preview_graphs(shader))
                for name in PREVIEW_IGNORED:
                    inp = shader.GetInput(name)
                    if not inp:
                        continue
                    value = inp.Get()
                    if inp.HasConnectedSource() or value == "transparent" or \
                            any(v != 0.0 for v in as_floats(value)):
                        ignored[name] += 1
        elif prim.HasAPI(UsdLux.LightAPI):
            lights[str(prim.GetTypeName()) or "LightAPI"] += 1
            for attr in prim.GetAuthoredAttributes():
                for prefix in LIGHT_ATTRIBUTES:
                    if attr.GetName().startswith(prefix) and \
                            (prefix != "inputs:enableColorTemperature" or attr.Get()):
                        light_attributes[prefix.rstrip(":")] += 1

    return {
        "stage": relative_name(path, root),
        "gprims": dict(gprims),
        "subdivision": dict(subdivision),
        "bound_gprims": dict(bindings),
        "materials": len(kinds),
        "surface_shaders": dict(surfaces),
        "lobes": {sid: dict(used) for sid, used in lobe_use.items()},
        "preview_graphs": dict(graphs),
        "preview_unread_inputs": dict(ignored),
        "textures": len(textures),
        "texture_formats": dict(collections.Counter(
            t.rsplit(".", 1)[-1].lower() for t in textures)),
        "udim_textures": sum("<UDIM>" in t for t in textures),
        "lights": dict(lights),
        "unsupported_lights": sorted(set(lights) - LOTUS_LIGHTS),
        "light_attributes": dict(light_attributes),
        "counts": dict(counts),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    stages = commands.add_parser("inventory", help="count features of USD stages")
    stages.add_argument("--root", required=True, help="directory paths are recorded relative to")
    stages.add_argument("--out", required=True, type=Path)
    stages.add_argument("stage", nargs="+")
    logs = commands.add_parser("warnings", help="classify Lotus warnings in usdrecord logs")
    logs.add_argument("--out", required=True, type=Path)
    logs.add_argument("log", nargs="+", type=Path)
    args = parser.parse_args(argv)

    if args.command == "inventory":
        result = [inventory(stage, args.root) for stage in args.stage]
    else:
        result = {log.stem: classify_log(log.read_text(encoding="utf-8", errors="replace"))
                  for log in args.log}
    args.out.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
