// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reelx P4 (fdHandoff): a ui.Image is read back exactly as toByteData would
// and written to a file descriptor (an encoder's input) on a writer thread
// of that descriptor, off the UI thread. Completions are posted to a Dart
// port in call order as [token, status, bytes, digest] (+ errno when a write
// failed). The last frame written to each descriptor is kept, so a reused
// frame is written again with no render or readback.

#include "flutter/lib/ui/reelx/reelx_ffi.h"

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

constexpr int64_t kStatusOk = 0;
constexpr int64_t kStatusReadbackFailed = 1;
constexpr int64_t kStatusWriteFailed = 2;

// 64-bit digest of a frame's bytes, compared only with other digests from
// this engine: FNV-1a over 8-byte words, then the tail, then a final mix.
uint64_t Digest(const uint8_t* data, size_t length) {
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

void Post(int64_t port, int64_t token, int64_t status, int64_t bytes,
          uint64_t digest, int64_t error) {
  Dart_CObject values[5];
  Dart_CObject* pointers[5];
  int64_t fields[5] = {token, status, bytes, static_cast<int64_t>(digest),
                       error};
  int count = status == kStatusWriteFailed ? 5 : 4;
  for (int i = 0; i < count; i++) {
    values[i].type = Dart_CObject_kInt64;
    values[i].value.as_int64 = fields[i];
    pointers[i] = &values[i];
  }
  Dart_CObject message;
  message.type = Dart_CObject_kArray;
  message.value.as_array.length = count;
  message.value.as_array.values = pointers;
  Dart_PostCObject(port, &message);
}

/// One descriptor's writer: requests are written in the order they were
/// made, each once its bytes are ready, on the writer's own thread.
class FdWriter : public std::enable_shared_from_this<FdWriter> {
 public:
  explicit FdWriter(int fd) : fd_(fd) {}

  void Start() {
    std::thread([self = shared_from_this()] { self->Run(); }).detach();
  }

  /// A slot for the next readback, in call order.
  uint64_t Reserve(int64_t port, int64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t slot = next_slot_++;
    slots_[slot] = Slot{port, token, false, false, nullptr};
    has_frames_ = true;
    return slot;
  }

  /// Queues a repeat of the frame written just before it (resolved when it
  /// is written, so a frame still pending is the one repeated). False when
  /// no frame was ever asked for on this descriptor.
  bool QueueRepeat(int64_t port, int64_t token) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!has_frames_) {
        return false;
      }
      slots_[next_slot_++] = Slot{port, token, true, true, nullptr};
    }
    cv_.notify_one();
    return true;
  }

  /// The bytes for [slot], or null when its readback failed.
  void Fulfil(uint64_t slot, sk_sp<SkData> data) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = slots_.find(slot);
      if (it == slots_.end()) {
        return;
      }
      it->second.ready = true;
      it->second.data = std::move(data);
    }
    cv_.notify_one();
  }

  /// No more requests: the thread writes or fails what is pending, then
  /// ends. Pending slots whose bytes never come are reported as failed.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_one();
  }

 private:
  struct Slot {
    int64_t port;
    int64_t token;
    bool ready;
    bool repeat;
    sk_sp<SkData> data;
  };

  void Run() {
    while (true) {
      Slot slot;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        // The next slot in call order once its bytes are ready; after Stop,
        // whatever is next (a slot still waiting for bytes fails).
        cv_.wait(lock, [this] {
          auto it = slots_.find(written_);
          return stopping_ || (it != slots_.end() && it->second.ready);
        });
        auto it = slots_.find(written_);
        if (it == slots_.end()) {
          return;  // Stopping with nothing pending.
        }
        slot = std::move(it->second);
        slots_.erase(it);
        written_++;
        if (slot.repeat) {
          slot.data = last_;
        }
      }
      if (!slot.ready || !slot.data) {
        Post(slot.port, slot.token, kStatusReadbackFailed, 0, 0, 0);
        continue;
      }
      const uint8_t* bytes = slot.data->bytes();
      size_t length = slot.data->size();
      size_t offset = 0;
      int error = 0;
      while (offset < length) {
        ssize_t n = write(fd_, bytes + offset, length - offset);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          error = n < 0 ? errno : EIO;
          break;
        }
        offset += static_cast<size_t>(n);
      }
      if (error != 0) {
        Post(slot.port, slot.token, kStatusWriteFailed, offset, 0, error);
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = slot.data;
      }
      Post(slot.port, slot.token, kStatusOk, length,
           Digest(bytes, length), 0);
    }
  }

  const int fd_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint64_t, Slot> slots_;
  uint64_t next_slot_ = 0;
  uint64_t written_ = 0;
  bool stopping_ = false;
  bool has_frames_ = false;
  sk_sp<SkData> last_;
};

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
  auto on_raster = [writer, slot, byte_format](
                       const fml::StatusOr<sk_sp<SkImage>>& raster) {
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
