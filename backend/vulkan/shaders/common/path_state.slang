// SPDX-License-Identifier: Apache-2.0
// The wavefront integrator's path state (wavefront/wavefront.slang): one
// slot per target pixel, which the kernels pass between their queues and
// the resolve pass (path_trace.slang) reads each pixel's sample from.
module path_state;

import common.scene;
import common.spectrum;

// Path state is stored by section: each section is an array of
// path_capacity 16-byte entries, one per slot, so a kernel loads and stores
// only the parts of PathState it uses rather than the whole state. A
// slot's pixel index is the slot itself, and the depth of a path in bounce
// b's queues is b, so neither is stored. No transport step sets flags yet,
// so they are not stored either.
public static const uint kOriginSection = 0;     // ray origin, TMin
public static const uint kDirectionSection = 1;  // ray direction, TMax
public static const uint kThroughputSection = 2; // throughput, last pdf
// Radiance and random-number state; once the path has ended, its sample.
public static const uint kRadianceSection = 3;
// Instance, primitive, barycentrics.
public static const uint kHitSection = 4;

[[vk::binding(7, 0)]] public RWStructuredBuffer<uint4> paths;

public uint4 LoadSection(uint section, uint slot) {
  return paths[section * path_constants.path_capacity + slot];
}

public void StoreSection(uint section, uint slot, uint4 value) {
  paths[section * path_constants.path_capacity + slot] = value;
}

// What an ended path leaves for its pixel, in the accumulation's form: its
// radiance and a weight of 1. A camera ray that misses leaves zero, which
// adds no sample.
public void StoreSample(uint slot, Spectrum radiance) {
  StoreSection(kRadianceSection, slot,
      asuint(float4(SpectrumToRgb(radiance), 1.0)));
}

public void StoreNoSample(uint slot) {
  StoreSection(kRadianceSection, slot, uint4(0));
}

public float4 LoadSample(uint slot) {
  return asfloat(LoadSection(kRadianceSection, slot));
}
