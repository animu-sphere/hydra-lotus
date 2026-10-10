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
// Path state persists in one slot per target pixel between kernels. Queues
// hold slot indices and are appended to with atomic counters; single-lane
// kernels turn the counters into the next kernel's indirect dispatch.
import common.scene;
import common.spectrum;
import common.transport;

// Which kernel a pipeline runs.
[vk::constant_id(0)] const uint kKernel = 0;
static const uint kGenerateKernel = 0;
static const uint kIntersectKernel = 1;
static const uint kShadeKernel = 2;
static const uint kAccumulateKernel = 3;
static const uint kPrepareKernel = 4;
static const uint kAdvanceKernel = 5;

// Mirrors kWavefrontGroupSize and kWavefrontMaxGroupsX in
// vulkan_backend.cpp: a dispatch covers its invocations with groups in rows
// of at most kMaxGroupsX.
static const uint kGroupSize = 64;
static const uint kMaxGroupsX = 65535;

// Words of queue_state; mirrors kQueueStateWords and its neighbours in
// vulkan_backend.cpp.
// The two ray queues' lengths; kCurrentRayQueue says which one is read.
static const uint kRayCount = 0;
static const uint kHitCount = 2;
static const uint kTerminatedCount = 3;
static const uint kCurrentRayQueue = 4;
// The bounce the ray queue holds rays of.
static const uint kIteration = 5;
// VkDispatchIndirectCommand, 16-byte aligned.
static const uint kRayArgs = 8;
static const uint kHitArgs = 12;
static const uint kTerminatedArgs = 16;
// Per bounce, how many paths entered the ray queue: queue occupancy.
static const uint kPathCounts = 20;

// queue_items holds four queues of path_capacity slots each.
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

void Push(uint queue, uint count_word, uint slot) {
  uint index;
  InterlockedAdd(queue_state[count_word], 1, index);
  queue_items[queue * path_constants.path_capacity + index] = slot;
}

uint QueueItem(uint queue, uint index) {
  return queue_items[queue * path_constants.path_capacity + index];
}

void WriteArgs(uint word, uint count) {
  const uint groups = (count + kGroupSize - 1) / kGroupSize;
  queue_state[word] = min(groups, kMaxGroupsX);
  queue_state[word + 1] = (groups + kMaxGroupsX - 1) / kMaxGroupsX;
  queue_state[word + 2] = 1;
}

// The dispatches that follow read the counts this writes.
void Prepare() {
  const uint current = queue_state[kCurrentRayQueue];
  const uint rays = queue_state[kRayCount + current];
  queue_state[kPathCounts + queue_state[kIteration]] = rays;
  WriteArgs(kRayArgs, rays);
  WriteArgs(kHitArgs, queue_state[kHitCount]);
  WriteArgs(kTerminatedArgs, queue_state[kTerminatedCount]);
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
  Push(kRayQueue, kRayCount, path.pixelIndex);
}

// A camera ray that misses adds no sample; a continuation ray that misses
// ends its path with the environment.
void Intersect(uint index) {
  const uint current = queue_state[kCurrentRayQueue];
  if (index >= queue_state[kRayCount + current]) {
    return;
  }
  const uint slot = QueueItem(kRayQueue + current, index);
  PathState path = LoadPath(slot);
  Hit hit;
  if (Trace(path.ray, path.rngState, hit)) {
    StorePath(slot, path, hit);
    Push(kHitQueue, kHitCount, slot);
  } else if (path.depth != 0) {
    MissPath(path);
    StorePath(slot, path, hit);
    Push(kTerminatedQueue, kTerminatedCount, slot);
  }
}

void Shade(uint index) {
  if (index >= queue_state[kHitCount]) {
    return;
  }
  const uint slot = QueueItem(kHitQueue, index);
  PathState path = LoadPath(slot);
  const Hit hit = paths[slot].hit;
  const bool continues = ScatterPath(path, hit);
  StorePath(slot, path, hit);
  if (continues) {
    const uint next = queue_state[kCurrentRayQueue] ^ 1;
    Push(kRayQueue + next, kRayCount + next, slot);
  } else {
    Push(kTerminatedQueue, kTerminatedCount, slot);
  }
}

// Each pixel has at most one path per frame, so no two invocations add to
// the same pixel.
void Accumulate(uint index) {
  if (index >= queue_state[kTerminatedCount]) {
    return;
  }
  const uint slot = QueueItem(kTerminatedQueue, index);
  const uint2 pixel =
      uint2(slot % path_constants.width, slot / path_constants.width);
  accumulation[pixel] +=
      float4(SpectrumToRgb(LoadSpectrum(paths[slot].radiance)), 1.0);
}

// The next bounce reads the queue the shade kernel filled; the drained ray
// queue and the hit queue start empty again.
void Advance() {
  const uint current = queue_state[kCurrentRayQueue];
  queue_state[kRayCount + current] = 0;
  queue_state[kHitCount] = 0;
  queue_state[kCurrentRayQueue] = current ^ 1;
  queue_state[kIteration] = queue_state[kIteration] + 1;
  Prepare();
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
  } else if (kKernel == kAccumulateKernel) {
    Accumulate(index);
  } else if (index == 0) {
    if (kKernel == kPrepareKernel) {
      Prepare();
    } else {
      Advance();
    }
  }
}
