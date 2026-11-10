// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reelx P4 (fdHandoff): a ui.Image is read back exactly as toByteData would
// and written to a file descriptor (an encoder's input) on a writer thread
// of that descriptor, off the UI thread. Completions are posted to a Dart
// port in call order as [token, status, bytes, digest] (+ errno when a write
// failed). The last frame written to each descriptor is kept, so a reused
// frame is written again with no render or readback.

#include "flutter/lib/ui/reelx/reelx_handoff.h"
#include "flutter/lib/ui/reelx/reelx_ffi.h"

// File descriptors and FIFOs are POSIX; on Windows Reelx writes to FFmpeg's
// stdin itself, so the hand-off is not built there (see the end of the file).
#if !defined(_WIN32)

#include <errno.h>
#include <unistd.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "flutter/fml/make_copyable.h"
#include "flutter/fml/status_or.h"
#include "flutter/lib/ui/io_manager.h"
#include "flutter/lib/ui/painting/image.h"
#include "flutter/lib/ui/painting/image_encoding.h"
#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/lib/ui/painting/image_encoding_impeller.h"
#endif  // IMPELLER_SUPPORTS_RENDERING
#include "flutter/lib/ui/painting/image_encoding_skia.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "third_party/dart/runtime/include/dart_api.h"
#include "third_party/dart/runtime/include/dart_native_api.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/tonic/converter/dart_converter.h"

namespace flutter {
namespace reelx {
namespace {

[[maybe_unused]] const bool kReelxFdHandoff = [] {
  SetAvailable(Feature::kFdHandoff);
  return true;
}();

std::mutex& WritersMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<int, std::shared_ptr<FdWriter>>& Writers() {
  static std::map<int, std::shared_ptr<FdWriter>> writers;
  return writers;
}

std::shared_ptr<FdWriter> WriterFor(int fd, bool create) {
  std::lock_guard<std::mutex> lock(WritersMutex());
  auto& writers = Writers();
  auto it = writers.find(fd);
  if (it != writers.end()) {
    return it->second;
  }
  if (!create) {
    return nullptr;
  }
  auto writer = std::make_shared<FdWriter>(fd);
  writer->Start();
  writers[fd] = writer;
  return writer;
}

}  // namespace

void PostToDartPort(int64_t port, const std::vector<int64_t>& message) {
  std::vector<Dart_CObject> values(message.size());
  std::vector<Dart_CObject*> pointers(message.size());
  for (size_t i = 0; i < message.size(); i++) {
    values[i].type = Dart_CObject_kInt64;
    values[i].value.as_int64 = message[i];
    pointers[i] = &values[i];
  }
  Dart_CObject array;
  array.type = Dart_CObject_kArray;
  array.value.as_array.length = static_cast<intptr_t>(message.size());
  array.value.as_array.values = pointers.data();
  Dart_PostCObject(port, &array);
}

uint64_t FrameDigest(const uint8_t* data, size_t length) {
  uint64_t hash = 0xcbf29ce484222325ull;
  size_t i = 0;
  for (; i + 8 <= length; i += 8) {
    uint64_t word;
    std::memcpy(&word, data + i, 8);
    hash = (hash ^ word) * 0x100000001b3ull;
  }
  for (; i < length; i++) {
    hash = (hash ^ data[i]) * 0x100000001b3ull;
  }
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdull;
  hash ^= hash >> 33;
  return hash;
}

}  // namespace reelx
}  // namespace flutter

using flutter::reelx::FdWriter;
using flutter::reelx::WriterFor;

extern "C" {

REELX_EXPORT int32_t ReelxImageReadbackToFd(Dart_Handle image,
                                            int32_t fd,
                                            int32_t format,
                                            int64_t port,
                                            int64_t token) {
  if (!flutter::reelx::IsFeatureOn(flutter::reelx::Feature::kFdHandoff)) {
    return -1;
  }
  Dart_Handle native = Dart_GetField(image, tonic::ToDart("_image"));
  if (Dart_IsError(native)) {
    return -1;
  }
  flutter::CanvasImage* canvas_image =
      tonic::DartConverter<flutter::CanvasImage*>::FromDart(native);
  if (!canvas_image || !canvas_image->image()) {
    return -1;
  }
  if (fd < 0) {
    return -2;
  }
  flutter::ImageByteFormat byte_format;
  switch (format) {
    case 0:
      byte_format = flutter::ImageByteFormat::kRawUnmodified;
      break;
    case 1:
      byte_format = flutter::ImageByteFormat::kRawRGBA;
      break;
    default:
      return -3;
  }

  std::shared_ptr<FdWriter> writer = WriterFor(fd, /*create=*/true);
  const uint64_t slot = writer->Reserve(port, token);

  flutter::UIDartState* state = flutter::UIDartState::Current();
  const flutter::TaskRunners& task_runners = state->GetTaskRunners();
  auto io_manager = state->GetIOManager();
  // Read back as toByteData does, then hand the bytes to the writer (from
  // whichever thread the readback finishes on).
  auto on_raster = [writer, slot,
                    byte_format](const fml::StatusOr<sk_sp<SkImage>>& raster) {
    if (!raster.ok()) {
      writer->Fulfil(slot, nullptr);
      return;
    }
    fml::StatusOr<sk_sp<SkData>> bytes =
        flutter::EncodeImage(raster.value(), byte_format);
    writer->Fulfil(slot, bytes.ok() ? bytes.value() : nullptr);
  };
  task_runners.GetIOTaskRunner()->PostTask(fml::MakeCopyable(
      [on_raster = std::move(on_raster), image = canvas_image->image(),
       raster_task_runner = task_runners.GetRasterTaskRunner(),
       io_task_runner = task_runners.GetIOTaskRunner(), io_manager,
       snapshot_delegate = state->GetSnapshotDelegate(),
       is_impeller_enabled = state->IsImpellerEnabled(), writer,
       slot]() mutable {
        if (!io_manager) {
          writer->Fulfil(slot, nullptr);
          return;
        }
#if IMPELLER_SUPPORTS_RENDERING
        if (is_impeller_enabled) {
          flutter::ImageEncodingImpeller::ConvertImageToRaster(
              image, on_raster, raster_task_runner, io_task_runner,
              snapshot_delegate, io_manager->GetIsGpuDisabledSyncSwitch(),
              io_manager->GetImpellerContext());
          return;
        }
#endif  // IMPELLER_SUPPORTS_RENDERING
#if !SLIMPELLER
        flutter::ConvertImageToRasterSkia(
            image, on_raster, raster_task_runner, io_task_runner,
            io_manager->GetResourceContext(), snapshot_delegate,
            io_manager->GetIsGpuDisabledSyncSwitch());
#else   // !SLIMPELLER
        writer->Fulfil(slot, nullptr);
#endif  // !SLIMPELLER
      }));
  return 0;
}

REELX_EXPORT int32_t ReelxRepeatLastToFd(int32_t fd,
                                         int64_t port,
                                         int64_t token) {
  if (!flutter::reelx::IsFeatureOn(flutter::reelx::Feature::kFdHandoff)) {
    return -1;
  }
  std::shared_ptr<FdWriter> writer = WriterFor(fd, /*create=*/false);
  if (!writer || !writer->QueueRepeat(port, token)) {
    return -4;
  }
  return 0;
}

REELX_EXPORT int32_t ReelxReleaseFd(int32_t fd) {
  std::shared_ptr<FdWriter> writer;
  {
    std::lock_guard<std::mutex> lock(flutter::reelx::WritersMutex());
    auto& writers = flutter::reelx::Writers();
    auto it = writers.find(fd);
    if (it == writers.end()) {
      return 0;
    }
    writer = std::move(it->second);
    writers.erase(it);
  }
  writer->Stop();
  return 0;
}

}  // extern "C"

#else  // defined(_WIN32)

extern "C" {

REELX_EXPORT int32_t ReelxImageReadbackToFd(Dart_Handle image,
                                            int32_t fd,
                                            int32_t format,
                                            int64_t port,
                                            int64_t token) {
  return -1;
}

REELX_EXPORT int32_t ReelxRepeatLastToFd(int32_t fd,
                                         int64_t port,
                                         int64_t token) {
  return -1;
}

REELX_EXPORT int32_t ReelxReleaseFd(int32_t fd) {
  return 0;
}

}  // extern "C"

#endif  // !defined(_WIN32)
