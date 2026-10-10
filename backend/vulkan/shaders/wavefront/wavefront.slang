// SPDX-License-Identifier: Apache-2.0
// The wavefront integrator (design policy section 7; roadmap policy,
// Renderer Phase 2): the reference transport (common/transport.slang) split
// into compute kernels that pass paths between queues instead of following
// each path to its end in one invocation.
//
//   camera → hit queue → shade → ray queue → intersect → hit queue → …
//                          │                     │
//                          └──→ ended: sample ←──┘ → resolve pass
//
// The camera kernel starts each pixel's path and traces its camera ray, so
// the first round needs no ray queue. After the recorded rounds, the tail
// kernel follows any path still in the ray queue to its end. Path state
// persists in one slot per target pixel (common/path_state.slang); an
// ended path leaves its sample in its slot, where the resolve pass adds it
// to the pixel's accumulation. Queues hold slot indices; an append keeps the
// queue's count and indirect dispatch current, so no kernel only does
// bookkeeping.
import common.path_state;
import common.scene;
import common.spectrum;
import common.transport;

// Which kernel a pipeline runs.
[vk::constant_id(0)] const uint kKernel = 0;
static const uint kCameraKernel = 0;
static const uint kIntersectKernel = 1;
static const uint kShadeKernel = 2;
static const uint kTailKernel = 3;

// Mirrors kWavefrontGroupSize and kWavefrontMaxGroupsX in
// vulkan_backend.cpp. The camera kernel covers its invocations with groups
// in rows of at most kMaxGroupsX; a queue's dispatch is one row of at most
// kMaxGroupsX groups.
static const uint kGroupSize = 64;
static const uint kMaxGroupsX = 65535;

// Words of queue_state; mirrors kQueueStateWords and its neighbours in
// vulkan_backend.cpp. A queue's descriptor is a VkDispatchIndirectCommand
// for its entries followed by their count.
static const uint kCountWord = 3;
// The bounces the next intersect and shade kernels run. Each kernel's first
// invocation hands its bounce on to the other kernel, so no kernel writes a
// word it reads. A round whose queue is empty runs no invocation, and
// neither do the rounds after it.
static const uint kIntersectBounce = 0;
static const uint kShadeBounce = 1;
// Per bounce, the ray queue's descriptor and then the hit queue's. The ray
// counts are the queue occupancy the host reads back; bounce 0 has no ray
// queue, and its count is the camera rays.
static const uint kBounces = 4;
static const uint kBounceWords = 8;

uint RayDescriptor(uint bounce) {
  return kBounces + bounce * kBounceWords;
}

uint HitDescriptor(uint bounce) {
  return kBounces + bounce * kBounceWords + 4;
}

// queue_items holds three queues of path_capacity slots each; bounces
// alternate between the two ray queues, bounce 0 using neither.
static const uint kRayQueue = 0;
static const uint kHitQueue = 2;

[[vk::binding(5, 0)]] RWStructuredBuffer<uint> queue_state;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> queue_items;

uint4 Pack(float3 xyz, float w) {
  return uint4(asuint(xyz), asuint(w));
}

void LoadRay(uint slot, inout PathState path) {
  const float4 origin = asfloat(LoadSection(kOriginSection, slot));
  const float4 direction = asfloat(LoadSection(kDirectionSection, slot));
  path.ray.Origin = origin.xyz;
  path.ray.TMin = origin.w;
  path.ray.Direction = direction.xyz;
  path.ray.TMax = direction.w;
}

// The state a path carries between bounces, everything but its hit.
PathState LoadPath(uint slot, uint depth) {
  PathState path;
  LoadRay(slot, path);
  const uint4 throughput = LoadSection(kThroughputSection, slot);
  path.throughput = UnpackSpectrum(throughput);
  path.lastPdf = asfloat(throughput.w);
  const uint4 radiance = LoadSection(kRadianceSection, slot);
  path.radiance = UnpackSpectrum(radiance);
  path.rngState = radiance.w;
  path.flags = 0;
  path.pixelIndex = slot;
  path.depth = depth;
  return path;
}

// What shading reads of a path: everything but its ray's origin and
// interval, which a scattering event replaces. A camera path's throughput
// and last density are the ones StartPath gives it, so neither is stored
// for it.
PathState LoadShadedPath(uint slot, uint depth) {
  PathState path;
  path.ray.Origin = float3(0.0);
  path.ray.TMin = 0.0;
  path.ray.Direction = asfloat(LoadSection(kDirectionSection, slot)).xyz;
  path.ray.TMax = 0.0;
  if (depth == 0) {
    path.throughput = SpectrumConstant(1.0);
    path.lastPdf = 0.0;
  } else {
    const uint4 throughput = LoadSection(kThroughputSection, slot);
    path.throughput = UnpackSpectrum(throughput);
    path.lastPdf = asfloat(throughput.w);
  }
  const uint4 radiance = LoadSection(kRadianceSection, slot);
  path.radiance = UnpackSpectrum(radiance);
  path.rngState = radiance.w;
  path.flags = 0;
  path.pixelIndex = slot;
  path.depth = depth;
  return path;
}

// What a path continues with: its ray, throughput, radiance and
// random-number state.
void StorePath(uint slot, PathState path) {
  StoreSection(kOriginSection, slot, Pack(path.ray.Origin, path.ray.TMin));
  StoreSection(kDirectionSection, slot,
      Pack(path.ray.Direction, path.ray.TMax));
  StoreSection(kThroughputSection, slot,
      PackSpectrum(path.throughput, asuint(path.lastPdf)));
  StoreRadiance(slot, path);
}

void StoreRadiance(uint slot, PathState path) {
  StoreSection(kRadianceSection, slot,
      PackSpectrum(path.radiance, path.rngState));
}

void StoreHit(uint slot, Hit hit) {
  StoreSection(kHitSection, slot,
      uint4(hit.instance, hit.primitive, asuint(hit.barycentrics)));
}

// Shading reads no hit distance, so none is stored.
Hit LoadHit(uint slot) {
  const uint4 stored = LoadSection(kHitSection, slot);
  Hit hit;
  hit.instance = stored.x;
  hit.primitive = stored.y;
  hit.barycentrics = asfloat(stored.zw);
  hit.t = 0.0;
  return hit;
}

uint Groups(uint count) {
  return min((count + kGroupSize - 1) / kGroupSize, kMaxGroupsX);
}

// Appends the slot to a queue, growing its dispatch by a group whenever
// the entry begins one.
void Push(uint descriptor, uint queue, uint slot) {
  uint index;
  InterlockedAdd(queue_state[descriptor + kCountWord], 1, index);
  const uint added = Groups(index + 1) - Groups(index);
  if (added != 0) {
    InterlockedAdd(queue_state[descriptor], added);
  }
  queue_items[queue * path_constants.path_capacity + index] = slot;
}

uint QueueItem(uint queue, uint index) {
  return queue_items[queue * path_constants.path_capacity + index];
}

// An invocation visits the queue's entry at its own index and, once the
// queue has outgrown kMaxGroupsX groups, every dispatch width after it. A
// queue is read only by kernels after the ones that append to it.
uint QueueCount(uint descriptor) {
  return queue_state[descriptor + kCountWord];
}

uint QueueStride(uint descriptor) {
  return queue_state[descriptor] * kGroupSize;
}

// A camera path per pixel of the data window, its camera ray traced at
// once: a hit is queued for shading with what shading reads of the path,
// and a miss leaves no sample.
void Camera(uint index) {
  const uint4 rect = path_constants.data_rect;
  if (path_constants.add_sample == 0 || index >= rect.z * rect.w) {
    return;
  }
  const uint2 pixel = rect.xy + uint2(index % rect.z, index / rect.z);
  PathState path = StartPath(pixel);
  const uint slot = path.pixelIndex;
  InterlockedAdd(queue_state[RayDescriptor(0) + kCountWord], 1);
  Hit hit;
  if (Trace(path.ray, path.rngState, hit)) {
    StoreSection(kDirectionSection, slot,
        Pack(path.ray.Direction, path.ray.TMax));
    StoreRadiance(slot, path);
    StoreHit(slot, hit);
    Push(HitDescriptor(0), kHitQueue, slot);
  } else {
    StoreNoSample(slot);
  }
}

// A continuation ray that misses ends its path with the environment.
// Tracing draws random numbers only at fractionally opaque candidates, so
// the random state is stored back only when it changed.
void Intersect(uint index) {
  const uint bounce = queue_state[kIntersectBounce];
  if (index == 0) {
    queue_state[kShadeBounce] = bounce;
  }
  const uint rays = RayDescriptor(bounce);
  const uint count = QueueCount(rays);
  const uint stride = QueueStride(rays);
  for (uint position = index; position < count; position += stride) {
    const uint slot = QueueItem(kRayQueue + (bounce & 1), position);
    PathState path;
    LoadRay(slot, path);
    const uint4 radiance = LoadSection(kRadianceSection, slot);
    path.radiance = UnpackSpectrum(radiance);
    path.rngState = radiance.w;
    Hit hit;
    if (Trace(path.ray, path.rngState, hit)) {
      StoreHit(slot, hit);
      Push(HitDescriptor(bounce), kHitQueue, slot);
      if (path.rngState != radiance.w) {
        StoreRadiance(slot, path);
      }
    } else {
      path.throughput =
          UnpackSpectrum(LoadSection(kThroughputSection, slot));
      MissPath(path);
      StoreSample(slot, path.radiance);
    }
  }
}

void Shade(uint index) {
  const uint bounce = queue_state[kShadeBounce];
  if (index == 0) {
    queue_state[kIntersectBounce] = bounce + 1;
  }
  const uint hits = HitDescriptor(bounce);
  const uint count = QueueCount(hits);
  const uint stride = QueueStride(hits);
  for (uint position = index; position < count; position += stride) {
    const uint slot = QueueItem(kHitQueue, position);
    PathState path = LoadShadedPath(slot, bounce);
    if (ScatterPath(path, LoadHit(slot))) {
      StorePath(slot, path);
      Push(RayDescriptor(bounce + 1), kRayQueue + ((bounce + 1) & 1), slot);
    } else {
      StoreSample(slot, path.radiance);
    }
  }
}

// The paths still in the ray queue after the recorded rounds, each followed
// to its end in one invocation, as the reference integrator does. The ray
// counts of the rounds it replaces still count the rays it traces.
void Tail(uint index) {
  const uint bounce = queue_state[kIntersectBounce];
  const uint rays = RayDescriptor(bounce);
  const uint count = QueueCount(rays);
  const uint stride = QueueStride(rays);
  for (uint position = index; position < count; position += stride) {
    const uint slot = QueueItem(kRayQueue + (bounce & 1), position);
    PathState path = LoadPath(slot, bounce);
    Hit hit;
    // At least one round ran, so every ray here continues a path.
    while (true) {
      if (!Trace(path.ray, path.rngState, hit)) {
        MissPath(path);
        break;
      }
      if (!ScatterPath(path, hit)) {
        break;
      }
      InterlockedAdd(queue_state[RayDescriptor(path.depth) + kCountWord], 1);
    }
    StoreSample(slot, path.radiance);
  }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void wavefront_main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
  const uint index = (group.y * kMaxGroupsX + group.x) * kGroupSize + lane;
  if (kKernel == kCameraKernel) {
    Camera(index);
  } else if (kKernel == kIntersectKernel) {
    Intersect(index);
  } else if (kKernel == kShadeKernel) {
    Shade(index);
  } else {
    Tail(index);
  }
}
