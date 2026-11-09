// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/reelx/reelx_ffi.h"

#include <unistd.h>

#include <string>

#include "gtest/gtest.h"

namespace flutter {
namespace reelx {
namespace testing {

namespace {

// Restores every flag after a test so tests stay independent.
class ReelxFfiTest : public ::testing::Test {
 protected:
  void SetUp() override { ResetFeaturesForTesting(); }
  void TearDown() override { ResetFeaturesForTesting(); }
};

std::string Info() {
  return std::string(ReelxEngineInfo());
}

}  // namespace

TEST_F(ReelxFfiTest, InfoHasTheContractKeys) {
  std::string info = Info();
  for (const char* key :
       {"\"flutter\":", "\"engineRevision\":", "\"patchSet\":",
        "\"forkCommit\":", "\"features\":{", "\"stats\":{"}) {
    EXPECT_NE(info.find(key), std::string::npos) << key << " in " << info;
  }
  for (const char* feature :
       {"lazyMips", "targetPool", "oneCopyReadback", "fdHandoff",
        "exactSnapshots", "bt709Textures", "testerMetal"}) {
    EXPECT_NE(info.find(std::string("\"") + feature + "\":"), std::string::npos)
        << feature;
  }
  EXPECT_EQ(info.front(), '{');
  EXPECT_EQ(info.back(), '}');
}

TEST_F(ReelxFfiTest, UnknownFeatureIsRejected) {
  EXPECT_EQ(ReelxSetFeature("noSuchFeature", 1), -1);
  EXPECT_EQ(ReelxSetFeature(nullptr, 1), -1);
}

TEST_F(ReelxFfiTest, FeatureNotInThisBuildStaysOff) {
  SetAvailableForTesting(Feature::kLazyMips, false);
  EXPECT_EQ(ReelxSetFeature("lazyMips", 1), -2);
  EXPECT_FALSE(IsFeatureOn(Feature::kLazyMips));
  EXPECT_NE(Info().find("\"lazyMips\":false"), std::string::npos);
}

TEST_F(ReelxFfiTest, AvailableFeatureCanBeSwitched) {
  SetAvailableForTesting(Feature::kTargetPool, true);
  EXPECT_TRUE(IsFeatureOn(Feature::kTargetPool));  // on by default
  EXPECT_NE(Info().find("\"targetPool\":true"), std::string::npos);

  EXPECT_EQ(ReelxSetFeature("targetPool", 0), 0);
  EXPECT_FALSE(IsFeatureOn(Feature::kTargetPool));
  EXPECT_NE(Info().find("\"targetPool\":false"), std::string::npos);

  EXPECT_EQ(ReelxSetFeature("targetPool", 1), 0);
  EXPECT_TRUE(IsFeatureOn(Feature::kTargetPool));
}

TEST_F(ReelxFfiTest, StatsAreCountedAndReported) {
  CountStat(Stat::kTargetPoolHits);
  CountStat(Stat::kTargetPoolHits);
  CountStat(Stat::kReadbackPoolMisses);
  std::string info = Info();
  EXPECT_NE(info.find("\"targetPoolHits\":2"), std::string::npos) << info;
  EXPECT_NE(info.find("\"readbackPoolMisses\":1"), std::string::npos) << info;
  EXPECT_NE(info.find("\"targetPoolMisses\":0"), std::string::npos) << info;
}

// P4: what the hand-off decides before any image or Dart port is involved
// (the readback path is covered by Reelx's gated export suite on the
// patched engine).
TEST_F(ReelxFfiTest, HandoffRepeatNeedsAFrameFirst) {
  SetAvailableForTesting(Feature::kFdHandoff, true);
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  EXPECT_EQ(ReelxRepeatLastToFd(fds[1], /*port=*/0, /*token=*/1), -4);
  EXPECT_EQ(ReelxReleaseFd(fds[1]), 0);
  close(fds[0]);
  close(fds[1]);
}

TEST_F(ReelxFfiTest, HandoffIsRefusedWhileTheFeatureIsOff) {
  SetAvailableForTesting(Feature::kFdHandoff, false);
  EXPECT_EQ(ReelxRepeatLastToFd(3, /*port=*/0, /*token=*/1), -1);
}

TEST_F(ReelxFfiTest, ReleasingAnUnknownDescriptorIsFine) {
  EXPECT_EQ(ReelxReleaseFd(12345), 0);
}

}  // namespace testing
}  // namespace reelx
}  // namespace flutter
