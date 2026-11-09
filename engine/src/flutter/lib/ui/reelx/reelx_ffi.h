// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_REELX_REELX_FFI_H_
#define FLUTTER_LIB_UI_REELX_REELX_FFI_H_

#include <cstdint>

#include "third_party/dart/runtime/include/dart_api.h"

// Reelx engine extensions.
//
// The C functions below are looked up from Dart with dart:ffi
// (DynamicLibrary.process()), so they must stay exported from the macOS
// framework and from flutter_tester (see common/exported_test_symbols*.sym).
// A stock engine doesn't have them; Reelx then keeps its default paths.

#if defined(_WIN32)
#define REELX_EXPORT __declspec(dllexport)
#else
#define REELX_EXPORT __attribute__((visibility("default"), used))
#endif

extern "C" {

/// JSON describing this engine build, the current feature switches and pool
/// counters. The string is owned by the engine and stays valid until the next
/// call on the same thread.
REELX_EXPORT const char* ReelxEngineInfo(void);

/// Turns the feature [name] on (on != 0) or off for the whole process.
/// Returns 0 on success, -1 for an unknown name, -2 for a feature this build
/// doesn't contain (it stays off).
REELX_EXPORT int32_t ReelxSetFeature(const char* name, int32_t on);

/// fdHandoff (reelx_handoff.cc). Reads [image] (a ui.Image) back as
/// toByteData would (format 0 rawUnmodified, 1 rawRgba) and writes the bytes
/// to [fd] on that descriptor's writer thread; posts [token, status, bytes,
/// digest] (+ errno when a write failed) to [port] in call order. Returns 0
/// when queued, -1 not an image (or the feature is off), -2 bad fd, -3
/// unknown format. The caller owns [fd] and may dispose [image] at once.
REELX_EXPORT int32_t ReelxImageReadbackToFd(Dart_Handle image,
                                            int32_t fd,
                                            int32_t format,
                                            int64_t port,
                                            int64_t token);

/// Writes the frame written just before it to [fd] again; -4 when nothing
/// was ever asked for on [fd].
REELX_EXPORT int32_t ReelxRepeatLastToFd(int32_t fd,
                                         int64_t port,
                                         int64_t token);

/// Ends [fd]'s writer once its pending writes end (requests still waiting
/// for bytes are reported as failed). The engine never closes [fd].
REELX_EXPORT int32_t ReelxReleaseFd(int32_t fd);

}  // extern "C"

namespace flutter {
namespace reelx {

/// Engine behaviours a Reelx patch adds. Each is on by default when the build
/// contains it and can be switched off with ReelxSetFeature.
enum class Feature : int {
  kLazyMips = 0,
  kTargetPool,
  kOneCopyReadback,
  kFdHandoff,
  kExactSnapshots,
  kBt709Textures,
  kTesterMetal,
  kCount,
};

/// Counters reported in ReelxEngineInfo's "stats".
enum class Stat : int {
  kTargetPoolHits = 0,
  kTargetPoolMisses,
  kReadbackPoolHits,
  kReadbackPoolMisses,
  kCount,
};

/// Whether [feature] is in this build and switched on.
bool IsFeatureOn(Feature feature);

/// Adds one to [stat].
void CountStat(Stat stat);

/// Marks [feature] as contained in this build. Each patch calls this once
/// from a static initializer next to the code it adds.
void SetAvailable(Feature feature);

/// Test hooks.
void SetAvailableForTesting(Feature feature, bool available);
void ResetFeaturesForTesting();

}  // namespace reelx
}  // namespace flutter

#endif  // FLUTTER_LIB_UI_REELX_REELX_FFI_H_
