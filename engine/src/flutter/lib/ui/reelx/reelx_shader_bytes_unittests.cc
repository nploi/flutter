// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/reelx/reelx_shader_bytes.h"

#include <vector>

#include "flutter/impeller/runtime_stage/runtime_stage.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"

namespace flutter {
namespace reelx {
namespace testing {

namespace {

class ReelxShaderBytesTest : public ::testing::Test {
 protected:
  void SetUp() override { ClearShaderBytesForTesting(); }
  void TearDown() override { ClearShaderBytesForTesting(); }
};

// An impellerc bundle built for the ui tests (fixtures/shaders).
std::vector<uint8_t> Bundle(const char* name = "filter_shader.frag.iplr") {
  auto mapping = flutter::testing::OpenFixtureAsMapping(name);
  FML_CHECK(mapping) << name;
  return std::vector<uint8_t>(mapping->GetMapping(),
                              mapping->GetMapping() + mapping->GetSize());
}

}  // namespace

TEST_F(ReelxShaderBytesTest, RegisteredBytesComeBackUnchanged) {
  std::vector<uint8_t> bundle = Bundle();
  ASSERT_EQ(RegisterShaderBytes("mem/fade.frag", bundle.data(), bundle.size()),
            kShaderBytesOk);
  auto mapping = GetRegisteredShader("mem/fade.frag");
  ASSERT_TRUE(mapping);
  EXPECT_EQ(std::vector<uint8_t>(mapping->GetMapping(),
                                 mapping->GetMapping() + mapping->GetSize()),
            bundle);
  EXPECT_FALSE(GetRegisteredShader("mem/other.frag"));
}

TEST_F(ReelxShaderBytesTest, TheMappingDecodesLikeTheAsset) {
  std::vector<uint8_t> bundle = Bundle();
  ASSERT_EQ(RegisterShaderBytes("k", bundle.data(), bundle.size()),
            kShaderBytesOk);
  std::shared_ptr<fml::Mapping> registered = GetRegisteredShader("k");
  std::shared_ptr<fml::Mapping> asset =
      flutter::testing::OpenFixtureAsMapping("filter_shader.frag.iplr");
  auto from_bytes = impeller::RuntimeStage::DecodeRuntimeStages(registered);
  auto from_asset = impeller::RuntimeStage::DecodeRuntimeStages(asset);
  ASSERT_TRUE(from_bytes.ok());
  ASSERT_TRUE(from_asset.ok());
  ASSERT_EQ(from_bytes->size(), from_asset->size());
  for (const auto& [backend, stage] : *from_asset) {
    ASSERT_TRUE(from_bytes->count(backend));
    if (!stage) {
      continue;
    }
    const auto& other = from_bytes->at(backend);
    ASSERT_TRUE(other);
    EXPECT_EQ(other->GetEntrypoint(), stage->GetEntrypoint());
    EXPECT_EQ(other->GetUniforms().size(), stage->GetUniforms().size());
  }
}

TEST_F(ReelxShaderBytesTest, BytesThatAreNotABundleAreRefused) {
  std::vector<uint8_t> junk = {'n', 'o', 't', ' ', 'a', ' ', 's', 'h',
                               'a', 'd', 'e', 'r', 0,   1,   2,   3};
  EXPECT_EQ(RegisterShaderBytes("junk", junk.data(), junk.size()),
            kShaderBytesNotABundle);
  // A bundle cut short is refused too.
  std::vector<uint8_t> bundle = Bundle();
  EXPECT_EQ(RegisterShaderBytes("cut", bundle.data(), bundle.size() / 2),
            kShaderBytesNotABundle);
  EXPECT_FALSE(GetRegisteredShader("junk"));
  EXPECT_FALSE(GetRegisteredShader("cut"));
}

TEST_F(ReelxShaderBytesTest, BadArgumentsAreRefused) {
  std::vector<uint8_t> bundle = Bundle();
  EXPECT_EQ(RegisterShaderBytes("", bundle.data(), bundle.size()),
            kShaderBytesBadArgument);
  EXPECT_EQ(RegisterShaderBytes("k", nullptr, 10), kShaderBytesBadArgument);
  EXPECT_EQ(RegisterShaderBytes("k", bundle.data(), 0),
            kShaderBytesBadArgument);
}

TEST_F(ReelxShaderBytesTest, ASecondRegistrationReplacesTheFirst) {
  std::vector<uint8_t> first = Bundle("filter_shader.frag.iplr");
  std::vector<uint8_t> second = Bundle("double_sampler.frag.iplr");
  ASSERT_NE(first, second);
  ASSERT_EQ(RegisterShaderBytes("k", first.data(), first.size()),
            kShaderBytesOk);
  ASSERT_EQ(RegisterShaderBytes("k", second.data(), second.size()),
            kShaderBytesOk);
  auto mapping = GetRegisteredShader("k");
  ASSERT_TRUE(mapping);
  EXPECT_EQ(mapping->GetSize(), second.size());
}

TEST_F(ReelxShaderBytesTest, UnregisterForgetsTheKeyButLiveMappingsStay) {
  std::vector<uint8_t> bundle = Bundle();
  ASSERT_EQ(RegisterShaderBytes("k", bundle.data(), bundle.size()),
            kShaderBytesOk);
  auto held = GetRegisteredShader("k");
  EXPECT_EQ(UnregisterShaderBytes("k"), kShaderBytesOk);
  EXPECT_EQ(UnregisterShaderBytes("k"), kShaderBytesNotRegistered);
  EXPECT_FALSE(GetRegisteredShader("k"));
  // The mapping handed out before still owns its bytes.
  ASSERT_TRUE(held);
  EXPECT_EQ(std::vector<uint8_t>(held->GetMapping(),
                                 held->GetMapping() + held->GetSize()),
            bundle);
}

}  // namespace testing
}  // namespace reelx
}  // namespace flutter
