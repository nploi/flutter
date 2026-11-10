// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reelx P4: the descriptor writer behind ReelxImageReadbackToFd.

#include "flutter/lib/ui/reelx/reelx_handoff.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include "gtest/gtest.h"

namespace flutter {
namespace reelx {
namespace testing {
namespace {

/// Collects what a writer posts, in order.
class Messages {
 public:
  HandoffPoster Poster() {
    return [this](int64_t port, const std::vector<int64_t>& message) {
      std::lock_guard<std::mutex> lock(mutex_);
      messages_.push_back(message);
      cv_.notify_all();
    };
  }

  /// Waits for [count] messages (5 s at most) and returns them.
  std::vector<std::vector<int64_t>> WaitFor(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::seconds(5),
                 [&] { return messages_.size() >= count; });
    return messages_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::vector<int64_t>> messages_;
};

sk_sp<SkData> Frame(uint8_t value, size_t size) {
  std::vector<uint8_t> bytes(size, value);
  return SkData::MakeWithCopy(bytes.data(), size);
}

std::vector<uint8_t> Read(int fd, size_t size) {
  std::vector<uint8_t> bytes(size);
  size_t got = 0;
  while (got < size) {
    ssize_t n = read(fd, bytes.data() + got, size - got);
    if (n <= 0) {
      break;
    }
    got += static_cast<size_t>(n);
  }
  bytes.resize(got);
  return bytes;
}

class ReelxHandoffTest : public ::testing::Test {
 protected:
  void SetUp() override {
    signal(SIGPIPE, SIG_IGN);
    ASSERT_EQ(pipe(fds_), 0);
  }
  void TearDown() override {
    if (fds_[0] >= 0) {
      close(fds_[0]);
    }
    close(fds_[1]);
  }
  int fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(ReelxHandoffTest, WritesInCallOrderWhateverOrderReadbacksFinish) {
  Messages messages;
  auto writer = std::make_shared<FdWriter>(fds_[1], messages.Poster());
  writer->Start();
  uint64_t a = writer->Reserve(0, 10);
  uint64_t b = writer->Reserve(0, 11);
  writer->Fulfil(b, Frame(2, 4));
  writer->Fulfil(a, Frame(1, 4));
  auto got = messages.WaitFor(2);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0][0], 10);
  EXPECT_EQ(got[1][0], 11);
  EXPECT_EQ(got[0][1], kHandoffOk);
  EXPECT_EQ(got[0][2], 4);
  EXPECT_EQ(Read(fds_[0], 8), (std::vector<uint8_t>{1, 1, 1, 1, 2, 2, 2, 2}));
  writer->Stop();
}

TEST_F(ReelxHandoffTest, ARepeatWritesThePreviousFrameEvenWhilePending) {
  Messages messages;
  auto writer = std::make_shared<FdWriter>(fds_[1], messages.Poster());
  writer->Start();
  EXPECT_FALSE(writer->QueueRepeat(0, 1)) << "nothing asked for yet";
  uint64_t a = writer->Reserve(0, 1);
  ASSERT_TRUE(writer->QueueRepeat(0, 2));  // frame 1 is still pending
  writer->Fulfil(a, Frame(7, 3));
  auto got = messages.WaitFor(2);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1][0], 2);
  EXPECT_EQ(got[1][1], kHandoffOk);
  EXPECT_EQ(got[0][3], got[1][3]) << "same frame, same digest";
  EXPECT_EQ(Read(fds_[0], 6), (std::vector<uint8_t>(6, 7)));
  writer->Stop();
}

TEST_F(ReelxHandoffTest, AFailedReadbackIsReportedAndTheNextFrameStillGoes) {
  Messages messages;
  auto writer = std::make_shared<FdWriter>(fds_[1], messages.Poster());
  writer->Start();
  writer->Fulfil(writer->Reserve(0, 1), nullptr);
  writer->Fulfil(writer->Reserve(0, 2), Frame(3, 2));
  auto got = messages.WaitFor(2);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0][1], kHandoffReadbackFailed);
  EXPECT_EQ(got[1][1], kHandoffOk);
  EXPECT_EQ(Read(fds_[0], 2), (std::vector<uint8_t>{3, 3}));
  writer->Stop();
}

TEST_F(ReelxHandoffTest, AGoneReaderIsAWriteFailureWithItsErrno) {
  Messages messages;
  auto writer = std::make_shared<FdWriter>(fds_[1], messages.Poster());
  writer->Start();
  close(fds_[0]);
  fds_[0] = -1;
  writer->Fulfil(writer->Reserve(0, 5), Frame(4, 1 << 20));
  auto got = messages.WaitFor(1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0][1], kHandoffWriteFailed);
  ASSERT_EQ(got[0].size(), 5u);
  EXPECT_EQ(got[0][4], EPIPE);
  writer->Stop();
}

TEST_F(ReelxHandoffTest, StoppingFailsWhatIsStillWaitingForBytes) {
  Messages messages;
  auto writer = std::make_shared<FdWriter>(fds_[1], messages.Poster());
  writer->Start();
  writer->Reserve(0, 9);
  writer->Stop();
  auto got = messages.WaitFor(1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0][0], 9);
  EXPECT_EQ(got[0][1], kHandoffReadbackFailed);
}

TEST(ReelxHandoffDigestTest, EqualBytesEqualDigests) {
  std::vector<uint8_t> a(37, 5);
  std::vector<uint8_t> b(37, 5);
  std::vector<uint8_t> c(37, 5);
  c[36] = 6;
  EXPECT_EQ(FrameDigest(a.data(), a.size()), FrameDigest(b.data(), b.size()));
  EXPECT_NE(FrameDigest(a.data(), a.size()), FrameDigest(c.data(), c.size()));
}

}  // namespace testing
}  // namespace reelx
}  // namespace flutter
