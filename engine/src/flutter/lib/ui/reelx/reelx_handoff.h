// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_REELX_REELX_HANDOFF_H_
#define FLUTTER_LIB_UI_REELX_REELX_HANDOFF_H_

// Reelx P4 (fdHandoff): the per-descriptor writer behind
// ReelxImageReadbackToFd and ReelxRepeatLastToFd (reelx_handoff.cc).

#include <errno.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/skia/include/core/SkData.h"

namespace flutter {
namespace reelx {

constexpr int64_t kHandoffOk = 0;
constexpr int64_t kHandoffReadbackFailed = 1;
constexpr int64_t kHandoffWriteFailed = 2;

/// Sends [message] ([token, status, bytes, digest], + errno when a write
/// failed) to [port].
using HandoffPoster =
    std::function<void(int64_t port, const std::vector<int64_t>& message)>;

/// Posts [message] to the Dart port [port] as an array of int64.
void PostToDartPort(int64_t port, const std::vector<int64_t>& message);

/// 64-bit digest of a frame's bytes, compared only with other digests from
/// this engine: FNV-1a over 8-byte words, then the tail, then a final mix.
uint64_t FrameDigest(const uint8_t* data, size_t length);

/// One descriptor's writer: requests are written in the order they were
/// made, each once its bytes are ready, on the writer's own thread.
class FdWriter : public std::enable_shared_from_this<FdWriter> {
 public:
  /// A writer for [fd] whose results go through [post] (to a Dart port by
  /// default; tests collect them).
  explicit FdWriter(int fd, HandoffPoster post = PostToDartPort)
      : fd_(fd), post_(std::move(post)) {}

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
        post_(slot.port, {slot.token, kHandoffReadbackFailed, 0, 0});
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
        post_(slot.port, {slot.token, kHandoffWriteFailed,
                          static_cast<int64_t>(offset), 0, error});
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = slot.data;
      }
      post_(slot.port, {slot.token, kHandoffOk, static_cast<int64_t>(length),
                        static_cast<int64_t>(FrameDigest(bytes, length))});
    }
  }

  const int fd_;
  const HandoffPoster post_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint64_t, Slot> slots_;
  uint64_t next_slot_ = 0;
  uint64_t written_ = 0;
  bool stopping_ = false;
  bool has_frames_ = false;
  sk_sp<SkData> last_;
};

}  // namespace reelx
}  // namespace flutter

#endif  // FLUTTER_LIB_UI_REELX_REELX_HANDOFF_H_
