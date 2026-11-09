// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reelx P1 (lazy mipmaps) and P3 (lossless snapshot targets).

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/dl_paint.h"
#include "flutter/impeller/display_list/aiks_unittests.h"
#include "gtest/gtest.h"
#include "impeller/core/sampler_descriptor.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/lazy_mipmaps.h"

namespace impeller {
namespace testing {

namespace {

sk_sp<flutter::DisplayList> TwoRects() {
  flutter::DisplayListBuilder builder;
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kRed());
  builder.DrawRect(flutter::DlRect::MakeXYWH(0, 0, 128, 256), paint);
  paint.setColor(flutter::DlColor::kBlue());
  builder.DrawRect(flutter::DlRect::MakeXYWH(128, 0, 128, 256), paint);
  return builder.Build();
}

SamplerDescriptor Sampler(MipFilter mip_filter) {
  SamplerDescriptor desc;
  desc.min_filter = desc.mag_filter = MinMagFilter::kLinear;
  desc.mip_filter = mip_filter;
  return desc;
}

}  // namespace

TEST_P(AiksTest, ReelxSnapshotWithoutMipsHasOneLevel) {
  AiksContext renderer(GetContext(), nullptr);
  auto eager = DisplayListToTexture(TwoRects(), {256, 256}, renderer,
                                    /*reset_host_buffer=*/true,
                                    /*generate_mips=*/true);
  auto lazy = DisplayListToTexture(TwoRects(), {256, 256}, renderer,
                                   /*reset_host_buffer=*/true,
                                   /*generate_mips=*/false);
  ASSERT_TRUE(eager && lazy);
  EXPECT_EQ(eager->GetMipCount(), ISize(256, 256).MipCount());
  EXPECT_GT(eager->GetMipCount(), 1u);
  EXPECT_EQ(lazy->GetMipCount(), 1u);
}

TEST_P(AiksTest, ReelxLazyMipmapsOnlyForMipmapSampling) {
  AiksContext renderer(GetContext(), nullptr);
  auto texture = DisplayListToTexture(TwoRects(), {256, 256}, renderer,
                                      /*reset_host_buffer=*/true,
                                      /*generate_mips=*/false);
  ASSERT_TRUE(texture);
  const ContentContext& content = renderer.GetContentContext();

  // Not marked: always itself.
  EXPECT_EQ(TextureForSampling(content, texture, Sampler(MipFilter::kLinear)),
            texture);

  texture->SetWantsLazyMipmaps(true);
  // A sampler that never reads mipmaps: itself, nothing made.
  EXPECT_EQ(TextureForSampling(content, texture, Sampler(MipFilter::kBase)),
            texture);
  EXPECT_EQ(TextureForSampling(content, texture, Sampler(MipFilter::kNearest)),
            texture);
  EXPECT_EQ(texture->GetLazyMipmapped(), nullptr);

  // Mipmap sampling: a mipmapped copy, made once.
  auto mipmapped =
      TextureForSampling(content, texture, Sampler(MipFilter::kLinear));
  ASSERT_TRUE(mipmapped);
  EXPECT_NE(mipmapped, texture);
  EXPECT_EQ(mipmapped->GetMipCount(), ISize(256, 256).MipCount());
  EXPECT_EQ(mipmapped->GetSize(), texture->GetSize());
  EXPECT_EQ(TextureForSampling(content, texture, Sampler(MipFilter::kLinear)),
            mipmapped);
}

TEST_P(AiksTest, ReelxLazyMipmapsNotForOneLevelSizes) {
  AiksContext renderer(GetContext(), nullptr);
  auto texture = DisplayListToTexture(TwoRects(), {1, 1}, renderer,
                                      /*reset_host_buffer=*/true,
                                      /*generate_mips=*/false);
  ASSERT_TRUE(texture);
  texture->SetWantsLazyMipmaps(true);
  EXPECT_FALSE(texture->WantsLazyMipmaps());
  EXPECT_EQ(TextureForSampling(renderer.GetContentContext(), texture,
                               Sampler(MipFilter::kLinear)),
            texture);
}

TEST_P(AiksTest, ReelxLosslessSnapshotTarget) {
  AiksContext renderer(GetContext(), nullptr);
  auto texture = DisplayListToTexture(TwoRects(), {256, 256}, renderer,
                                      /*reset_host_buffer=*/true,
                                      /*generate_mips=*/false,
                                      /*target_pixel_format=*/std::nullopt,
                                      /*lossless_target=*/true);
  ASSERT_TRUE(texture);
  EXPECT_EQ(texture->GetTextureDescriptor().compression_type,
            CompressionType::kLossless);
}

TEST_P(AiksTest, ReelxPooledTargetIsReusedOnceReleased) {
  AiksContext renderer(GetContext(), nullptr);
  auto snapshot = [&](bool* hit) {
    return DisplayListToTexture(TwoRects(), {256, 256}, renderer,
                                /*reset_host_buffer=*/true,
                                /*generate_mips=*/false,
                                /*target_pixel_format=*/std::nullopt,
                                /*lossless_target=*/false,
                                /*pooled_target=*/true, hit);
  };
  bool hit = true;
  auto first = snapshot(&hit);
  ASSERT_TRUE(first);
  EXPECT_FALSE(hit);
  Texture* first_address = first.get();

  // Still held: a second target.
  auto second = snapshot(&hit);
  ASSERT_TRUE(second);
  EXPECT_FALSE(hit);
  EXPECT_NE(second.get(), first_address);

  // Released: reused, with what the last snapshot left on it gone.
  first->SetWantsLazyMipmaps(true);
  first.reset();
  auto third = snapshot(&hit);
  EXPECT_TRUE(hit);
  EXPECT_EQ(third.get(), first_address);
  EXPECT_FALSE(third->WantsLazyMipmaps());
}

TEST_P(AiksTest, ReelxMipmappedTargetsAreNotPooled) {
  AiksContext renderer(GetContext(), nullptr);
  bool hit = true;
  auto texture = DisplayListToTexture(TwoRects(), {256, 256}, renderer,
                                      /*reset_host_buffer=*/true,
                                      /*generate_mips=*/true,
                                      /*target_pixel_format=*/std::nullopt,
                                      /*lossless_target=*/false,
                                      /*pooled_target=*/true, &hit);
  ASSERT_TRUE(texture);
  EXPECT_TRUE(renderer.GetContentContext().GetSnapshotTargetPool().empty());
}

}  // namespace testing
}  // namespace impeller
