// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/engine_perf/engine_perf_ffi.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <string>

#include "gtest/gtest.h"

namespace flutter {
namespace engine_perf {
namespace testing {

namespace {

// Restores every flag after a test so tests stay independent.
class EnginePerfFfiTest : public ::testing::Test {
 protected:
  void SetUp() override { ResetFeaturesForTesting(); }
  void TearDown() override { ResetFeaturesForTesting(); }
};

std::string Info() {
  return std::string(EnginePerfInfo());
}

}  // namespace

TEST_F(EnginePerfFfiTest, InfoHasTheContractKeys) {
  std::string info = Info();
  for (const char* key :
       {"\"flutter\":", "\"engineRevision\":", "\"patchSet\":",
        "\"forkCommit\":", "\"features\":{", "\"stats\":{"}) {
    EXPECT_NE(info.find(key), std::string::npos) << key << " in " << info;
  }
  for (const char* feature :
       {"lazyMips", "targetPool", "oneCopyReadback", "fdHandoff",
        "exactSnapshots", "bt709Textures", "testerMetal", "snapshotBuffers",
        "targetAttachments", "lazyImageMips", "singleSampleSnapshots"}) {
    EXPECT_NE(info.find(std::string("\"") + feature + "\":"), std::string::npos)
        << feature;
  }
  EXPECT_EQ(info.front(), '{');
  EXPECT_EQ(info.back(), '}');
}

TEST_F(EnginePerfFfiTest, UnknownFeatureIsRejected) {
  EXPECT_EQ(EnginePerfSetFeature("noSuchFeature", 1), -1);
  EXPECT_EQ(EnginePerfSetFeature(nullptr, 1), -1);
}

TEST_F(EnginePerfFfiTest, FeatureNotInThisBuildStaysOff) {
  SetAvailableForTesting(Feature::kLazyMips, false);
  EXPECT_EQ(EnginePerfSetFeature("lazyMips", 1), -2);
  EXPECT_FALSE(IsFeatureOn(Feature::kLazyMips));
  EXPECT_NE(Info().find("\"lazyMips\":false"), std::string::npos);
}

TEST_F(EnginePerfFfiTest, AvailableFeatureCanBeSwitched) {
  SetAvailableForTesting(Feature::kTargetPool, true);
  EXPECT_TRUE(IsFeatureOn(Feature::kTargetPool));  // on by default
  EXPECT_NE(Info().find("\"targetPool\":true"), std::string::npos);

  EXPECT_EQ(EnginePerfSetFeature("targetPool", 0), 0);
  EXPECT_FALSE(IsFeatureOn(Feature::kTargetPool));
  EXPECT_NE(Info().find("\"targetPool\":false"), std::string::npos);

  EXPECT_EQ(EnginePerfSetFeature("targetPool", 1), 0);
  EXPECT_TRUE(IsFeatureOn(Feature::kTargetPool));
}

TEST_F(EnginePerfFfiTest, StatsAreCountedAndReported) {
  CountStat(Stat::kTargetPoolHits);
  CountStat(Stat::kTargetPoolHits);
  CountStat(Stat::kReadbackPoolMisses);
  std::string info = Info();
  EXPECT_NE(info.find("\"targetPoolHits\":2"), std::string::npos) << info;
  EXPECT_NE(info.find("\"readbackPoolMisses\":1"), std::string::npos) << info;
  EXPECT_NE(info.find("\"targetPoolMisses\":0"), std::string::npos) << info;
}

// P4: what the hand-off decides before any image or Dart port is involved
// (the readback path is covered by end-to-end tests on the patched
// engine).
#if !defined(_WIN32)
TEST_F(EnginePerfFfiTest, HandoffRepeatNeedsAFrameFirst) {
  SetAvailableForTesting(Feature::kFdHandoff, true);
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  EXPECT_EQ(EnginePerfRepeatLastToFd(fds[1], /*port=*/0, /*token=*/1), -4);
  EXPECT_EQ(EnginePerfReleaseFd(fds[1]), 0);
  close(fds[0]);
  close(fds[1]);
}

TEST_F(EnginePerfFfiTest, HandoffIsRefusedWhileTheFeatureIsOff) {
  SetAvailableForTesting(Feature::kFdHandoff, false);
  EXPECT_EQ(EnginePerfRepeatLastToFd(3, /*port=*/0, /*token=*/1), -1);
}

TEST_F(EnginePerfFfiTest, ReleasingAnUnknownDescriptorIsFine) {
  EXPECT_EQ(EnginePerfReleaseFd(12345), 0);
}
#endif  // !defined(_WIN32)

}  // namespace testing
}  // namespace engine_perf
}  // namespace flutter
