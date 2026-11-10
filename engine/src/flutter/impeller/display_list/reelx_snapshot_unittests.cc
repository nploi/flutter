// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reelx P1 (lazy mipmaps) and P3 (lossless snapshot targets).

#include <cstring>
#include <string>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/dl_paint.h"
#include "flutter/impeller/display_list/aiks_unittests.h"
#include "fml/synchronization/count_down_latch.h"
#include "gtest/gtest.h"
#include "impeller/core/device_buffer.h"
#include "impeller/core/sampler_descriptor.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/display_list/lazy_mipmaps.h"
#include "impeller/renderer/command_buffer.h"

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

// Stripes 3 px wide (a period of 6, which 8 doesn't divide): drawn at 1/8
// they blend smoothly with mipmaps and alias without them.
sk_sp<flutter::DisplayList> Stripes() {
  flutter::DisplayListBuilder builder;
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kBlack());
  builder.DrawRect(flutter::DlRect::MakeWH(256, 256), paint);
  paint.setColor(flutter::DlColor::kWhite());
  for (int x = 0; x < 256; x += 6) {
    builder.DrawRect(flutter::DlRect::MakeXYWH(x, 0, 3, 256), paint);
  }
  return builder.Build();
}

std::vector<uint8_t> ReadBack(const std::shared_ptr<Context>& context,
                              const std::shared_ptr<Texture>& texture) {
  DeviceBufferDescriptor desc;
  desc.size = texture->GetTextureDescriptor().GetByteSizeOfBaseMipLevel();
  desc.readback = true;
  desc.storage_mode = StorageMode::kHostVisible;
  auto buffer = context->GetResourceAllocator()->CreateBuffer(desc);
  auto cmd_buffer = context->CreateCommandBuffer();
  auto blit_pass = cmd_buffer->CreateBlitPass();
  blit_pass->AddCopy(texture, buffer);
  blit_pass->EncodeCommands();
  auto latch = std::make_shared<fml::CountDownLatch>(1u);
  context->GetCommandQueue()->Submit(
      {cmd_buffer},
      [latch](CommandBuffer::Status status) { latch->CountDown(); });
  latch->Wait();
  buffer->Invalidate();
  const uint8_t* bytes = buffer->OnGetContents();
  return std::vector<uint8_t>(bytes, bytes + desc.size);
}

// [image] drawn at 32 x 32 with [sampling], read back.
std::vector<uint8_t> DrawnSmall(const std::shared_ptr<Context>& context,
                                AiksContext& renderer,
                                const std::shared_ptr<Texture>& image,
                                flutter::DlImageSampling sampling) {
  flutter::DisplayListBuilder builder;
  builder.DrawImageRect(DlImageImpeller::Make(image),
                        flutter::DlRect::MakeWH(256, 256),
                        flutter::DlRect::MakeWH(32, 32), sampling);
  auto small = DisplayListToTexture(builder.Build(), {32, 32}, renderer);
  return ReadBack(context, small);
}

int MaxDifference(const std::vector<uint8_t>& a,
                  const std::vector<uint8_t>& b) {
  int most = a.size() == b.size() ? 0 : 255;
  for (size_t i = 0; i < a.size() && i < b.size(); i++) {
    most = std::max(most, std::abs(static_cast<int>(a[i]) - b[i]));
  }
  return most;
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

// A reused target draws the new snapshot exactly as a fresh one does.
TEST_P(AiksTest, ReelxReusedTargetDrawsLikeAFreshOne) {
  AiksContext renderer(GetContext(), nullptr);
  auto snapshot = [&](const sk_sp<flutter::DisplayList>& list, bool pooled,
                      bool* hit) {
    return DisplayListToTexture(list, {256, 256}, renderer,
                                /*reset_host_buffer=*/true,
                                /*generate_mips=*/false,
                                /*target_pixel_format=*/std::nullopt,
                                /*lossless_target=*/false, pooled, hit);
  };
  bool hit = true;
  auto first = snapshot(TwoRects(), /*pooled=*/true, &hit);
  ASSERT_TRUE(first);
  ReadBack(GetContext(), first);
  first.reset();

  auto reused = snapshot(Stripes(), /*pooled=*/true, &hit);
  ASSERT_TRUE(reused);
  EXPECT_TRUE(hit);
  auto fresh = snapshot(Stripes(), /*pooled=*/false, nullptr);
  ASSERT_TRUE(fresh);
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), reused),
                          ReadBack(GetContext(), fresh)),
            0);
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

// P1: a lazily mipmapped snapshot drawn at 1/8 with mipmaps gives the same
// pixels as one whose mipmaps were made when it was rendered.
TEST_P(AiksTest, ReelxLazyMipmapsDrawLikeEagerOnes) {
  AiksContext renderer(GetContext(), nullptr);
  auto eager = DisplayListToTexture(Stripes(), {256, 256}, renderer,
                                    /*reset_host_buffer=*/true,
                                    /*generate_mips=*/true);
  auto lazy = DisplayListToTexture(Stripes(), {256, 256}, renderer,
                                   /*reset_host_buffer=*/true,
                                   /*generate_mips=*/false);
  ASSERT_TRUE(eager && lazy);
  lazy->SetWantsLazyMipmaps(true);

  auto from_eager = DrawnSmall(GetContext(), renderer, eager,
                               flutter::DlImageSampling::kMipmapLinear);
  auto from_lazy = DrawnSmall(GetContext(), renderer, lazy,
                              flutter::DlImageSampling::kMipmapLinear);
  auto mipmapped = lazy->GetLazyMipmapped();
  ASSERT_NE(mipmapped, nullptr);
  // The mipmaps matter here: without them the stripes alias.
  auto unfiltered = DrawnSmall(GetContext(), renderer, eager,
                               flutter::DlImageSampling::kLinear);
  EXPECT_GT(MaxDifference(from_eager, unfiltered), 8);

  // When the lazy draw differs, these say where: the snapshots themselves,
  // the copy's base level, its mipmaps (the copy drawn directly), or the draw
  // that made the copy.
  auto from_copy = DrawnSmall(GetContext(), renderer, mipmapped,
                              flutter::DlImageSampling::kMipmapLinear);
  auto row = [](const std::vector<uint8_t> &pixels) {
    std::string s;
    for (size_t i = 0; i < 32 * 4 && i < pixels.size(); i += 4) {
      s += std::to_string(pixels[i]) + " ";
    }
    return s;
  };
  const TextureDescriptor &copy_desc = mipmapped->GetTextureDescriptor();
  EXPECT_EQ(copy_desc.mip_count, ISize(256, 256).MipCount());
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), eager),
                          ReadBack(GetContext(), lazy)),
            0)
      << "the two snapshots differ";
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), mipmapped),
                          ReadBack(GetContext(), lazy)),
            0)
      << "the copy's base level differs from the snapshot";
  EXPECT_LE(MaxDifference(from_eager, from_copy), 1)
      << "the copy drawn directly differs: its mipmaps are wrong";
  EXPECT_LE(MaxDifference(from_eager, from_lazy), 1)
      << "\n eager      " << row(from_eager) //
      << "\n lazy       " << row(from_lazy)  //
      << "\n copy       " << row(from_copy)  //
      << "\n unfiltered " << row(unfiltered) //
      << "\n copy storage " << static_cast<int>(copy_desc.storage_mode)
      << " compression " << static_cast<int>(copy_desc.compression_type)
      << " usage "
      << static_cast<int>(
             static_cast<TextureUsageMask::MaskType>(copy_desc.usage))
      << " format " << static_cast<int>(copy_desc.format);
}

}  // namespace testing
}  // namespace impeller
