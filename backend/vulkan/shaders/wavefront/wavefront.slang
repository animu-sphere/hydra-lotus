// SPDX-License-Identifier: Apache-2.0
// The wavefront integrator (design policy section 7; roadmap policy,
// Renderer Phase 2): the reference transport (common/transport.slang) split
// into compute kernels that pass paths between queues instead of following
// each path to its end in one invocation.
//
//   generate → ray queue → intersect → hit queue → shade → next ray queue
//                              │                     │
//                              └──→ terminated ←─────┘ → accumulate
//
// After the recorded rounds, the tail kernel follows any path still in the
// ray queue to its end. Path state persists in one slot per target pixel
// between kernels. Queues hold slot indices; an append keeps the queue's
// count and indirect dispatch current, so no kernel only does bookkeeping.
import common.scene;
import common.spectrum;
import common.transport;

// Which kernel a pipeline runs.
[vk::constant_id(0)] const uint kKernel = 0;
static const uint kGenerateKernel = 0;
static const uint kIntersectKernel = 1;
static const uint kShadeKernel = 2;
static const uint kTailKernel = 3;
static const uint kAccumulateKernel = 4;

// Mirrors kWavefrontGroupSize and kWavefrontMaxGroupsX in
// vulkan_backend.cpp. Generate covers its invocations with groups in rows
// of at most kMaxGroupsX; a queue's dispatch is one row of at most
// kMaxGroupsX groups.
static const uint kGroupSize = 64;
static const uint kMaxGroupsX = 65535;

// Words of queue_state; mirrors kQueueStateWords and its neighbours in
// vulkan_backend.cpp. A queue's descriptor is a VkDispatchIndirectCommand
// for its entries followed by their count.
static const uint kCountWord = 3;
// The terminated paths' descriptor.
static const uint kTerminated = 0;
// The bounces the next intersect and shade kernels run. Each kernel's first
// invocation hands its bounce on to the other kernel, so no kernel writes a
// word it reads. A round whose queue is empty runs no invocation, and
// neither do the rounds after it.
static const uint kIntersectBounce = 4;
static const uint kShadeBounce = 5;
// Per bounce, the ray queue's descriptor and then the hit queue's. The ray
// counts are the queue occupancy the host reads back.
static const uint kBounces = 8;
static const uint kBounceWords = 8;

uint RayDescriptor(uint bounce) {
  return kBounces + bounce * kBounceWords;
}

uint HitDescriptor(uint bounce) {
  return kBounces + bounce * kBounceWords + 4;
}

// queue_items holds four queues of path_capacity slots each; bounces
// alternate between the two ray queues.
static const uint kRayQueue = 0;
static const uint kHitQueue = 2;
static const uint kTerminatedQueue = 3;

// One path slot: PathState and the hit its ray found. No member is a
// float3 (see StoredSpectrum).
struct PathRecord {
  float4 origin_tmin;
  float4 direction_tmax;
  StoredSpectrum throughput;
  StoredSpectrum radiance;
  uint pixel_index;
  uint depth;
  uint rng;
  float last_pdf;
  uint flags;
  Hit hit;
};

[[vk::binding(5, 0)]] RWStructuredBuffer<uint> queue_state;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> queue_items;
[[vk::binding(7, 0)]] RWStructuredBuffer<PathRecord> paths;

PathState LoadPath(uint slot) {
  const PathRecord record = paths[slot];
  PathState path;
  path.ray.Origin = record.origin_tmin.xyz;
  path.ray.TMin = record.origin_tmin.w;
  path.ray.Direction = record.direction_tmax.xyz;
  path.ray.TMax = record.direction_tmax.w;
  path.throughput = LoadSpectrum(record.throughput);
  path.radiance = LoadSpectrum(record.radiance);
  path.pixelIndex = record.pixel_index;
  path.depth = record.depth;
  path.rngState = record.rng;
  path.lastPdf = record.last_pdf;
  path.flags = record.flags;
  return path;
}

void StorePath(uint slot, PathState path, Hit hit) {
  PathRecord record;
  record.origin_tmin = float4(path.ray.Origin, path.ray.TMin);
  record.direction_tmax = float4(path.ray.Direction, path.ray.TMax);
  record.throughput = StoreSpectrum(path.throughput);
  record.radiance = StoreSpectrum(path.radiance);
  record.pixel_index = path.pixelIndex;
  record.depth = path.depth;
  record.rng = path.rngState;
  record.last_pdf = path.lastPdf;
  record.flags = path.flags;
  record.hit = hit;
  paths[slot] = record;
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

// A camera path per pixel of the data window, queued for intersection. A new
// accumulation starts from zero here, at every pixel the resolve pass reads.
void Generate(uint index) {
  const uint4 rect = path_constants.data_rect;
  if (path_constants.add_sample == 0 || index >= rect.z * rect.w) {
    return;
  }
  const uint2 pixel = rect.xy + uint2(index % rect.z, index / rect.z);
  if (path_constants.accumulated == 0) {
    accumulation[pixel] = float4(0.0);
  }
  const PathState path = StartPath(pixel);
  StorePath(path.pixelIndex, path, (Hit)0);
  Push(RayDescriptor(0), kRayQueue, path.pixelIndex);
}

// A camera ray that misses adds no sample; a continuation ray that misses
// ends its path with the environment.
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
    PathState path = LoadPath(slot);
    Hit hit;
    if (Trace(path.ray, path.rngState, hit)) {
      StorePath(slot, path, hit);
      Push(HitDescriptor(bounce), kHitQueue, slot);
    } else if (path.depth != 0) {
      MissPath(path);
      StorePath(slot, path, hit);
      Push(kTerminated, kTerminatedQueue, slot);
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
    PathState path = LoadPath(slot);
    const Hit hit = paths[slot].hit;
    const bool continues = ScatterPath(path, hit);
    StorePath(slot, path, hit);
    if (continues) {
      Push(RayDescriptor(bounce + 1), kRayQueue + ((bounce + 1) & 1), slot);
    } else {
      Push(kTerminated, kTerminatedQueue, slot);
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
    PathState path = LoadPath(slot);
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
    StorePath(slot, path, hit);
    Push(kTerminated, kTerminatedQueue, slot);
  }
}

// Each pixel has at most one path per frame, so no two invocations add to
// the same pixel.
void Accumulate(uint index) {
  const uint count = QueueCount(kTerminated);
  const uint stride = QueueStride(kTerminated);
  for (uint position = index; position < count; position += stride) {
    const uint slot = QueueItem(kTerminatedQueue, position);
    const uint2 pixel =
        uint2(slot % path_constants.width, slot / path_constants.width);
    accumulation[pixel] +=
        float4(SpectrumToRgb(LoadSpectrum(paths[slot].radiance)), 1.0);
  }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void wavefront_main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
  const uint index = (group.y * kMaxGroupsX + group.x) * kGroupSize + lane;
  if (kKernel == kGenerateKernel) {
    Generate(index);
  } else if (kKernel == kIntersectKernel) {
    Intersect(index);
  } else if (kKernel == kShadeKernel) {
    Shade(index);
  } else if (kKernel == kTailKernel) {
    Tail(index);
  } else {
    Accumulate(index);
  }
}
