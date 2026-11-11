// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/engine_perf/engine_perf_ffi.h"

#include <array>
#include <atomic>
#include <cstring>
#include <string>

#if IMPELLER_SUPPORTS_RENDERING
#include "impeller/display_list/lazy_mipmaps.h"
#endif  // IMPELLER_SUPPORTS_RENDERING

// Written by engine_perf/tools/build_engine.sh before a build; absent in a
// plain checkout, where the build reports "dev".
#if __has_include("flutter/lib/ui/engine_perf/engine_perf_build_info.h")
#include "flutter/lib/ui/engine_perf/engine_perf_build_info.h"
#endif
#ifndef ENGINE_PERF_FLUTTER_VERSION
#define ENGINE_PERF_FLUTTER_VERSION "dev"
#endif
#ifndef ENGINE_PERF_ENGINE_REVISION
#define ENGINE_PERF_ENGINE_REVISION "dev"
#endif
#ifndef ENGINE_PERF_PATCH_SET
#define ENGINE_PERF_PATCH_SET "dev"
#endif
#ifndef ENGINE_PERF_FORK_COMMIT
#define ENGINE_PERF_FORK_COMMIT "dev"
#endif

namespace flutter {
namespace engine_perf {
namespace {

constexpr int kFeatureCount = static_cast<int>(Feature::kCount);
constexpr int kStatCount = static_cast<int>(Stat::kCount);

// Names as they appear in EnginePerfInfo and EnginePerfSetFeature.
constexpr std::array<const char*, kFeatureCount> kFeatureNames = {
    "lazyMips",
    "targetPool",
    "oneCopyReadback",
    "fdHandoff",
    "exactSnapshots",
    "bt709Textures",
    "testerMetal",
    "snapshotBuffers",
    "targetAttachments",
    "lazyImageMips",
    "singleSampleSnapshots",
};

constexpr std::array<const char*, kStatCount> kStatNames = {
    "targetPoolHits",       "targetPoolMisses",       "readbackPoolHits",
    "readbackPoolMisses",   "snapshotBufferResets",   "snapshotBufferHolds",
    "targetAttachmentHits", "targetAttachmentMisses", "imageMipsDeferred",
    "imageMipsGenerated",   "singleSampleSnapshots",
};

struct State {
  // What this build contains (set by each patch), what tests pretend it
  // contains, and what is switched on.
  std::array<std::atomic<bool>, kFeatureCount> built{};
  std::array<std::atomic<bool>, kFeatureCount> available{};
  std::array<std::atomic<bool>, kFeatureCount> on{};
  std::array<std::atomic<uint64_t>, kStatCount> stats{};
};

State& GetState() {
  static State state;
  return state;
}

int FeatureIndex(const char* name) {
  if (name == nullptr) {
    return -1;
  }
  for (int i = 0; i < kFeatureCount; i++) {
    if (std::strcmp(name, kFeatureNames[i]) == 0) {
      return i;
    }
  }
  return -1;
}

}  // namespace

bool IsFeatureOn(Feature feature) {
  int i = static_cast<int>(feature);
  State& state = GetState();
  return state.available[i].load(std::memory_order_relaxed) &&
         state.on[i].load(std::memory_order_relaxed);
}

void CountStat(Stat stat) {
  GetState().stats[static_cast<int>(stat)].fetch_add(1,
                                                     std::memory_order_relaxed);
}

void SetAvailable(Feature feature) {
  int i = static_cast<int>(feature);
  State& state = GetState();
  state.built[i].store(true);
  state.available[i].store(true);
  state.on[i].store(true);
}

void SetAvailableForTesting(Feature feature, bool available) {
  int i = static_cast<int>(feature);
  State& state = GetState();
  state.available[i].store(available);
  state.on[i].store(available);
}

void ResetFeaturesForTesting() {
  State& state = GetState();
  for (int i = 0; i < kFeatureCount; i++) {
    bool built = state.built[i].load();
    state.available[i].store(built);
    state.on[i].store(built);
  }
  for (auto& stat : state.stats) {
    stat.store(0);
  }
}

}  // namespace engine_perf
}  // namespace flutter

extern "C" {

const char* EnginePerfInfo(void) {
  using flutter::engine_perf::GetState;
  using flutter::engine_perf::kFeatureNames;
  using flutter::engine_perf::kStatNames;
  auto& state = flutter::engine_perf::GetState();

  thread_local std::string json;
  json = "{\"flutter\":\"" ENGINE_PERF_FLUTTER_VERSION
         "\",\"engineRevision\":\"" ENGINE_PERF_ENGINE_REVISION
         "\",\"patchSet\":\"" ENGINE_PERF_PATCH_SET
         "\",\"forkCommit\":\"" ENGINE_PERF_FORK_COMMIT "\",\"features\":{";
  for (size_t i = 0; i < kFeatureNames.size(); i++) {
    bool on = state.available[i].load() && state.on[i].load();
    json += (i ? ",\"" : "\"");
    json += kFeatureNames[i];
    json += on ? "\":true" : "\":false";
  }
  json += "},\"stats\":{";
#if IMPELLER_SUPPORTS_RENDERING
  // Counted where the mipmaps are filled, in Impeller.
  state.stats[static_cast<int>(flutter::engine_perf::Stat::kImageMipsGenerated)]
      .store(impeller::GetDeferredMipmapsGenerated());
#endif  // IMPELLER_SUPPORTS_RENDERING
  for (size_t i = 0; i < kStatNames.size(); i++) {
    json += (i ? ",\"" : "\"");
    json += kStatNames[i];
    json += "\":" + std::to_string(state.stats[i].load());
  }
  json += "}}";
  return json.c_str();
}

int32_t EnginePerfSetFeature(const char* name, int32_t on) {
  int i = flutter::engine_perf::FeatureIndex(name);
  if (i < 0) {
    return -1;
  }
  auto& state = flutter::engine_perf::GetState();
  if (!state.available[i].load()) {
    return -2;
  }
  state.on[i].store(on != 0);
  return 0;
}

}  // extern "C"
