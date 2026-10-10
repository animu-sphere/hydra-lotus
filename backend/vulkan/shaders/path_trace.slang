// SPDX-License-Identifier: Apache-2.0
// The reference path tracer (design policy section 9) as fullscreen fragment
// passes. The camera pass traces each pixel centre for depth and the
// barycentric intersection diagnostic, or in the surface pass a diagnostic
// of the hit's surface: its shading normal, albedo, or roughness and
// metallic. The radiance pass adds one camera path per pixel, through a
// box-filtered point of the pixel, to a floating-point accumulation image
// and writes the accumulated mean: the reference integrator, which follows
// each path to its end in one loop (common/transport.slang). The resolve
// pass does the same with the sample the wavefront integrator's path left
// in the pixel's slot (common/path_state.slang).
import common.path_state;
import common.scene;
import sampling.sampling;
import common.spectrum;
import common.transport;

// Which pass a pipeline runs: kCameraPass, kRadiancePass, kSurfacePass or
// kResolvePass.
[vk::constant_id(0)] const uint kPass = 0;
static const uint kCameraPass = 0;
static const uint kRadiancePass = 1;
static const uint kSurfacePass = 2;
static const uint kResolvePass = 3;
static const uint kShadingNormalOutput = 0;
static const uint kAlbedoOutput = 1;
static const uint kRoughnessMetallicOutput = 2;

// The radiance a camera path carries back, given its first hit.
Spectrum TracePath(inout PathState path, Hit hit) {
  while (ScatterPath(path, hit)) {
    if (!Trace(path.ray, path.rngState, hit)) {
      MissPath(path);
      break;
    }
  }
  return path.radiance;
}

struct VertexOutput {
  float4 position : SV_Position;
  float2 ndc : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint index : SV_VertexID) {
  VertexOutput output;
  float2 ndc = float2(index == 1 ? 3.0 : -1.0,
                     index == 2 ? 3.0 : -1.0);
  output.position = float4(ndc, 0.0, 1.0);
  output.ndc = ndc;
  return output;
}

struct FragmentOutput {
  float4 color : SV_Target;
  float depth : SV_Depth;
};

// The camera pass: the pixel centre's closest hit, as its projected depth
// and barycentric weights, or in the surface pass a diagnostic of its
// surface. Misses keep the attachments.
FragmentOutput CameraPass(VertexOutput input) {
  const RayDesc ray = CameraRay(input.ndc);
  // Pixel-centre depth and diagnostics use a fixed coverage realization,
  // independent of the progressive radiance sample and prior frames.
  const uint2 pixel = uint2(input.position.xy);
  uint rng = SeedRandom(pixel.y * path_constants.width + pixel.x, 0);
  Hit hit;
  if (!Trace(ray, rng, hit)) {
    discard;
  }
  float3 position = ray.Origin + ray.Direction * hit.t;
  float4 clip = mul(frame.world_to_clip, float4(position, 1.0));
  FragmentOutput output;
  output.depth = clip.z / clip.w;
  if (kPass == kSurfacePass) {
    const SurfaceHit surface = ReconstructHit(hit, ray.Direction);
    if (path_constants.surface_output == kAlbedoOutput) {
      output.color = float4(surface.base_color, 1.0);
    } else if (path_constants.surface_output == kRoughnessMetallicOutput) {
      output.color = float4(surface.roughness, surface.metallic, 0.0, 1.0);
    } else {
      output.color = float4(surface.shading_normal, 1.0);
    }
  } else {
    output.color = float4(1.0 - hit.barycentrics.x - hit.barycentrics.y,
        hit.barycentrics.x, hit.barycentrics.y, 1.0);
  }
  return output;
}

// The radiance pass: adds one sample to the pixel's accumulation and writes
// the mean of its samples, a missed sample counting as the background. A
// pixel no sample has hit keeps the colour attachment. Depth is the camera
// pass's; this pipeline neither tests nor writes it. The resolve pass adds
// the sample the wavefront kernels left instead of tracing one.
FragmentOutput RadiancePass(VertexOutput input) {
  const uint2 pixel = uint2(input.position.xy);
  float4 sum = path_constants.accumulated != 0 ? accumulation[pixel]
                                               : float4(0.0);
  if (path_constants.add_sample != 0) {
    if (kPass == kResolvePass) {
      sum += LoadSample(pixel.y * path_constants.width + pixel.x);
    } else {
      PathState path = StartPath(pixel);
      Hit hit;
      if (Trace(path.ray, path.rngState, hit)) {
        sum += float4(SpectrumToRgb(TracePath(path, hit)), 1.0);
      }
    }
    accumulation[pixel] = sum;
  }
  if (sum.w == 0.0) {
    discard;
  }
  const float count =
      float(path_constants.accumulated + path_constants.add_sample);
  const float4 background = path_constants.background;
  FragmentOutput output;
  output.color = float4(sum.xyz + (count - sum.w) * background.xyz,
                     sum.w + (count - sum.w) * background.w) / count;
  output.depth = 1.0;
  return output;
}

[shader("fragment")]
FragmentOutput fragment_main(VertexOutput input) {
  if (kPass == kRadiancePass || kPass == kResolvePass) {
    return RadiancePass(input);
  }
  return CameraPass(input);
}
