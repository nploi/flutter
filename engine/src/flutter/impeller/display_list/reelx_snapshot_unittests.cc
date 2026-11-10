// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reelx P1 (lazy mipmaps), P3 (lossless snapshot targets) and P13 (snapshot
// host buffers).

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/effects/dl_color_filter.h"
#include "flutter/display_list/effects/dl_color_source.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/impeller/display_list/aiks_unittests.h"
#include "fml/synchronization/count_down_latch.h"
#include "gtest/gtest.h"
#include "impeller/core/device_buffer.h"
#include "impeller/core/host_buffer.h"
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

// Enough draws that a few snapshots' uniforms and vertices fill a host
// buffer block.
sk_sp<flutter::DisplayList> ManyRects() {
  flutter::DisplayListBuilder builder;
  flutter::DlPaint paint;
  for (int i = 0; i < 400; i++) {
    paint.setColor(flutter::DlColor::RGBA((i % 7) / 6.0f, (i % 5) / 4.0f,
                                          (i % 3) / 2.0f, 1.0f));
    builder.DrawRect(
        flutter::DlRect::MakeXYWH((i % 20) * 12, (i / 20) * 12, 10, 10), paint);
  }
  return builder.Build();
}

// Clips inside clips, a rounded one among them: draws that need the
// depth/stencil attachment, at any size.
sk_sp<flutter::DisplayList> ClippedRects(ISize size) {
  const float w = size.width;
  const float h = size.height;
  flutter::DisplayListBuilder builder;
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kGreen());
  builder.DrawRect(flutter::DlRect::MakeWH(w, h), paint);
  builder.Save();
  builder.ClipRoundRect(
      flutter::DlRoundRect::MakeRectXY(
          flutter::DlRect::MakeXYWH(w / 8, h / 8, w * 3 / 4, h * 3 / 4), w / 6,
          h / 6),
      flutter::DlClipOp::kIntersect, /*is_aa=*/false);
  paint.setColor(flutter::DlColor::kRed());
  builder.DrawRect(flutter::DlRect::MakeWH(w, h), paint);
  builder.ClipRect(flutter::DlRect::MakeXYWH(w / 4, h / 4, w / 4, h / 2),
                   flutter::DlClipOp::kDifference, /*is_aa=*/false);
  paint.setColor(flutter::DlColor::kBlue());
  builder.DrawCircle(flutter::DlPoint(w / 2, h / 2), w / 2, paint);
  builder.Restore();
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

// [texture]'s base level uploaded to a new texture with every mipmap level,
// the way a decoded image is. [deferred] leaves the levels above 0 unfilled
// and says so on the texture, as P15's upload does.
std::shared_ptr<Texture> Uploaded(const std::shared_ptr<Context>& context,
                                  const std::shared_ptr<Texture>& texture,
                                  bool deferred) {
  std::vector<uint8_t> bytes = ReadBack(context, texture);
  TextureDescriptor desc;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.format = texture->GetTextureDescriptor().format;
  desc.size = texture->GetSize();
  desc.mip_count = desc.size.MipCount();
  auto uploaded = context->GetResourceAllocator()->CreateTexture(desc);
  auto buffer = context->GetResourceAllocator()->CreateBufferWithCopy(
      bytes.data(), bytes.size());
  if (!uploaded || !buffer) {
    return nullptr;
  }
  auto cmd_buffer = context->CreateCommandBuffer();
  auto blit_pass = cmd_buffer->CreateBlitPass();
  if (!blit_pass->AddCopy(DeviceBuffer::AsBufferView(buffer), uploaded) ||
      (!deferred && !blit_pass->GenerateMipmap(uploaded)) ||
      !blit_pass->EncodeCommands()) {
    return nullptr;
  }
  auto latch = std::make_shared<fml::CountDownLatch>(1u);
  context->GetCommandQueue()->Submit(
      {cmd_buffer},
      [latch](CommandBuffer::Status status) { latch->CountDown(); });
  latch->Wait();
  uploaded->SetDeferredMipmaps(deferred);
  return uploaded;
}

std::shared_ptr<Texture> UploadedWithMipmaps(
    const std::shared_ptr<Context>& context,
    const std::shared_ptr<Texture>& texture) {
  return Uploaded(context, texture, /*deferred=*/false);
}

// [list] drawn at [size], read back.
std::vector<uint8_t> Drawn(const std::shared_ptr<Context>& context,
                           AiksContext& renderer,
                           const sk_sp<flutter::DisplayList>& list,
                           ISize size) {
  return ReadBack(context, DisplayListToTexture(list, size, renderer));
}

// [image] (256 x 256) drawn into a square of [extent] with [sampling].
sk_sp<flutter::DisplayList> ImageAt(const std::shared_ptr<Texture>& image,
                                    Scalar extent,
                                    flutter::DlImageSampling sampling,
                                    const flutter::DlPaint* paint = nullptr) {
  flutter::DisplayListBuilder builder;
  builder.DrawImageRect(
      DlImageImpeller::Make(image), flutter::DlRect::MakeWH(256, 256),
      flutter::DlRect::MakeWH(extent, extent), sampling, paint);
  return builder.Build();
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
  EXPECT_TRUE(
      renderer.GetContentContext().GetSnapshotTargetPool().targets.empty());
}

// P1: a lazily mipmapped snapshot drawn at 1/8 with mipmaps gives the same
// pixels as its own base level uploaded with mipmaps. The reference is not a
// snapshot rendered with mipmaps: on GitHub's macOS runner (Apple silicon,
// paravirtual GPU) such a snapshot's base level differs from the same display
// list rendered without mipmaps (by 255, at the left edge), which isn't P1's
// doing.
TEST_P(AiksTest, ReelxLazyMipmapsDrawLikeGeneratedOnes) {
  AiksContext renderer(GetContext(), nullptr);
  auto lazy = DisplayListToTexture(Stripes(), {256, 256}, renderer,
                                   /*reset_host_buffer=*/true,
                                   /*generate_mips=*/false);
  ASSERT_TRUE(lazy);
  auto uploaded = UploadedWithMipmaps(GetContext(), lazy);
  ASSERT_TRUE(uploaded);
  lazy->SetWantsLazyMipmaps(true);

  auto expected = DrawnSmall(GetContext(), renderer, uploaded,
                             flutter::DlImageSampling::kMipmapLinear);
  auto from_lazy = DrawnSmall(GetContext(), renderer, lazy,
                              flutter::DlImageSampling::kMipmapLinear);
  auto mipmapped = lazy->GetLazyMipmapped();
  ASSERT_NE(mipmapped, nullptr);
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), mipmapped),
                          ReadBack(GetContext(), lazy)),
            0)
      << "the copy's base level differs from the snapshot";
  auto row = [](const std::vector<uint8_t>& pixels) {
    std::string s;
    for (size_t i = 0; i < 32 * 4 && i < pixels.size(); i += 4) {
      s += std::to_string(pixels[i]) + " ";
    }
    return s;
  };
  EXPECT_LE(MaxDifference(expected, from_lazy), 1)
      << "\n expected " << row(expected) << "\n lazy     " << row(from_lazy);

  // The mipmaps matter here: without them the stripes alias.
  auto unfiltered = DrawnSmall(GetContext(), renderer, uploaded,
                               flutter::DlImageSampling::kLinear);
  EXPECT_GT(MaxDifference(expected, unfiltered), 8);
}

// P13: snapshots that draw with host buffers of their own reset them, so
// what they emplace doesn't pile up; the frames' buffers aren't touched, and
// the pixels are those of a snapshot drawn with the frames' buffers.
TEST_P(AiksTest, ReelxSnapshotsResetHostBuffersOfTheirOwn) {
  AiksContext renderer(GetContext(), nullptr);
  ContentContext& content = renderer.GetContentContext();
  auto snapshot = [&](bool own_buffers, bool* reset) {
    return DisplayListToTexture(ManyRects(), {256, 256}, renderer,
                                /*reset_host_buffer=*/false,
                                /*generate_mips=*/false,
                                /*target_pixel_format=*/std::nullopt,
                                /*lossless_target=*/false,
                                /*pooled_target=*/false, /*pool_hit=*/nullptr,
                                own_buffers, reset);
  };
  auto blocks = [](HostBuffer& buffer) {
    HostBuffer::TestStateQuery state = buffer.GetStateForTest();
    return std::max(state.current_buffer + 1, state.total_buffer_count);
  };

  HostBuffer* frames_buffer = &content.GetTransientsDataBuffer();
  auto stock = snapshot(/*own_buffers=*/false, nullptr);
  ASSERT_TRUE(stock);
  const std::vector<uint8_t> expected = ReadBack(GetContext(), stock);
  const size_t frames_blocks = blocks(*frames_buffer);
  const BufferView before = frames_buffer->Emplace(uint8_t{1});

  // Enough snapshots to come round the buffers several times. Reading each
  // back waits for its work, as an export does.
  constexpr int kSnapshots = 24;
  for (int i = 0; i < kSnapshots; i++) {
    bool reset = false;
    auto texture = snapshot(/*own_buffers=*/true, &reset);
    ASSERT_TRUE(texture);
    EXPECT_TRUE(reset) << "snapshot " << i;
    EXPECT_EQ(MaxDifference(ReadBack(GetContext(), texture), expected), 0)
        << "snapshot " << i;
  }

  // The frames' buffers are back and nothing was emplaced into them.
  EXPECT_EQ(&content.GetTransientsDataBuffer(), frames_buffer);
  EXPECT_EQ(frames_buffer->GetStateForTest().current_frame, 0u);
  EXPECT_EQ(blocks(*frames_buffer), frames_blocks);
  const BufferView after = frames_buffer->Emplace(uint8_t{1});
  EXPECT_EQ(after.GetBuffer(), before.GetBuffer());
  EXPECT_EQ(after.GetRange().offset, before.GetRange().offset + 1);

  // The snapshots' buffers hold no more than one snapshot needs.
  content.UseSnapshotTransientsBuffers(true);
  HostBuffer* snapshots_buffer = &content.GetTransientsDataBuffer();
  EXPECT_NE(snapshots_buffer, frames_buffer);
  const size_t snapshots_blocks = blocks(*snapshots_buffer);
  content.UseSnapshotTransientsBuffers(false);

  // Without them the same snapshots pile up in the frames' buffers, which
  // only a frame resets.
  for (int i = 0; i < kSnapshots; i++) {
    bool reset = true;
    ASSERT_TRUE(snapshot(/*own_buffers=*/false, &reset));
    EXPECT_FALSE(reset);
  }
  EXPECT_GT(blocks(*frames_buffer), frames_blocks);
  EXPECT_LE(snapshots_blocks, frames_blocks);
}

// P14: snapshots of one size draw with one MSAA colour and one depth/stencil
// texture, whether their own target is new or reused, and give the pixels of
// snapshots that made their own.
TEST_P(AiksTest, ReelxSnapshotsShareAttachments) {
  AiksContext renderer(GetContext(), nullptr);
  const SnapshotTargetPool& pool =
      renderer.GetContentContext().GetSnapshotTargetPool();
  const ISize size(256, 256);
  auto snapshot = [&](const sk_sp<flutter::DisplayList>& list, bool pooled,
                      bool* target_hit, bool* attachments_hit) {
    return DisplayListToTexture(
        list, size, renderer, /*reset_host_buffer=*/true,
        /*generate_mips=*/false, /*target_pixel_format=*/std::nullopt,
        /*lossless_target=*/false, /*pooled_target=*/pooled, target_hit,
        /*snapshot_host_buffers=*/false, /*host_buffers_reset=*/nullptr,
        /*pooled_attachments=*/pooled, attachments_hit);
  };
  const std::vector<uint8_t> clipped = ReadBack(
      GetContext(), snapshot(ClippedRects(size), false, nullptr, nullptr));
  const std::vector<uint8_t> stripes =
      ReadBack(GetContext(), snapshot(Stripes(), false, nullptr, nullptr));
  ASSERT_NE(MaxDifference(clipped, stripes), 0);
  EXPECT_TRUE(pool.attachments.empty());

  bool target_hit = true;
  bool attachments_hit = true;
  auto first =
      snapshot(ClippedRects(size), true, &target_hit, &attachments_hit);
  ASSERT_TRUE(first);
  EXPECT_FALSE(target_hit);
  EXPECT_FALSE(attachments_hit);
  ASSERT_EQ(pool.attachments.size(), 1u);
  const SnapshotAttachments kept = pool.attachments.front();
  EXPECT_NE(kept.id, 0u);
  ASSERT_TRUE(kept.depth_stencil);
  EXPECT_EQ(kept.depth_stencil->GetSize(), size);
  const bool msaa = GetContext()->GetCapabilities()->SupportsOffscreenMSAA();
  EXPECT_EQ(kept.color_msaa != nullptr, msaa);
  EXPECT_EQ(kept.depth_stencil->GetTextureDescriptor().sample_count,
            msaa ? SampleCount::kCount4 : SampleCount::kCount1);

  // A second target while the first is held: its own texture, the same
  // attachments. The first keeps its pixels.
  auto second = snapshot(Stripes(), true, &target_hit, &attachments_hit);
  ASSERT_TRUE(second);
  EXPECT_FALSE(target_hit);
  EXPECT_TRUE(attachments_hit);
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), second), stripes), 0);
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), first), clipped), 0);

  // Reused targets, each drawn again and again with what the backend
  // cached for it.
  for (int i = 0; i < 6; i++) {
    const bool odd = i % 2 == 1;
    Texture* released = (odd ? second : first).get();
    (odd ? second : first).reset();
    auto reused = snapshot(odd ? ClippedRects(size) : Stripes(), true,
                           &target_hit, &attachments_hit);
    ASSERT_TRUE(reused);
    EXPECT_TRUE(target_hit) << i;
    EXPECT_TRUE(attachments_hit) << i;
    EXPECT_EQ(reused.get(), released) << i;
    EXPECT_EQ(
        MaxDifference(ReadBack(GetContext(), reused), odd ? clipped : stripes),
        0)
        << i;
    (odd ? second : first) = reused;
  }
  ASSERT_EQ(pool.attachments.size(), 1u);
  EXPECT_EQ(pool.attachments.front().id, kept.id);
  EXPECT_EQ(pool.attachments.front().color_msaa, kept.color_msaa);
  EXPECT_EQ(pool.attachments.front().depth_stencil, kept.depth_stencil);
  for (const SnapshotTarget& target : pool.targets) {
    EXPECT_EQ(target.attachments_id, kept.id);
  }

  // Without pooled attachments the pool lets them go, and a reused target
  // draws with textures of its own again.
  first.reset();
  auto without = DisplayListToTexture(
      ClippedRects(size), size, renderer, /*reset_host_buffer=*/true,
      /*generate_mips=*/false, /*target_pixel_format=*/std::nullopt,
      /*lossless_target=*/false, /*pooled_target=*/true, &target_hit);
  ASSERT_TRUE(without);
  EXPECT_TRUE(target_hit);
  EXPECT_TRUE(pool.attachments.empty());
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), without), clipped), 0);
}

// P14: the pool keeps the attachments of the two sizes used last. A target
// reused after its size's attachments were replaced draws with the new ones.
TEST_P(AiksTest, ReelxAttachmentsFollowTheSnapshotSize) {
  AiksContext renderer(GetContext(), nullptr);
  const SnapshotTargetPool& pool =
      renderer.GetContentContext().GetSnapshotTargetPool();
  auto snapshot = [&](ISize size, bool pooled, bool* attachments_hit) {
    return DisplayListToTexture(
        ClippedRects(size), size, renderer, /*reset_host_buffer=*/true,
        /*generate_mips=*/false, /*target_pixel_format=*/std::nullopt,
        /*lossless_target=*/false, /*pooled_target=*/pooled,
        /*pool_hit=*/nullptr, /*snapshot_host_buffers=*/false,
        /*host_buffers_reset=*/nullptr, /*pooled_attachments=*/pooled,
        attachments_hit);
  };
  const ISize large(256, 256);
  const ISize medium(200, 120);
  const ISize small(64, 64);
  auto expected = [&](ISize size) {
    return ReadBack(GetContext(), snapshot(size, false, nullptr));
  };
  const std::vector<uint8_t> large_pixels = expected(large);
  const std::vector<uint8_t> medium_pixels = expected(medium);
  const std::vector<uint8_t> small_pixels = expected(small);
  auto check = [&](ISize size, const std::vector<uint8_t>& pixels,
                   bool expect_hit) {
    bool hit = !expect_hit;
    auto texture = snapshot(size, true, &hit);
    ASSERT_TRUE(texture);
    EXPECT_EQ(hit, expect_hit) << size.width;
    EXPECT_EQ(MaxDifference(ReadBack(GetContext(), texture), pixels), 0)
        << size.width;
    ASSERT_FALSE(pool.attachments.empty());
    EXPECT_EQ(pool.attachments.back().size, size);
    EXPECT_EQ(pool.attachments.back().depth_stencil->GetSize(), size);
  };

  // An export's two sizes, alternating: one miss each.
  check(large, large_pixels, false);
  check(medium, medium_pixels, false);
  for (int i = 0; i < 3; i++) {
    check(large, large_pixels, true);
    check(medium, medium_pixels, true);
  }
  EXPECT_EQ(pool.attachments.size(), 2u);
  const uint64_t large_id = pool.attachments.front().id;

  // A third size takes the place of the one used longest ago.
  check(small, small_pixels, false);
  EXPECT_EQ(pool.attachments.size(), 2u);
  EXPECT_EQ(pool.attachments.front().size, medium);
  check(medium, medium_pixels, true);

  // The large target is still pooled; its attachments are new.
  check(large, large_pixels, false);
  EXPECT_NE(pool.attachments.back().id, large_id);
  check(large, large_pixels, true);
}

// P14: after enough onscreen frames without a snapshot the pool holds only
// what something else holds too.
TEST_P(AiksTest, ReelxIdleSnapshotPoolLetsGo) {
  AiksContext renderer(GetContext(), nullptr);
  ContentContext& content = renderer.GetContentContext();
  const SnapshotTargetPool& pool = content.GetSnapshotTargetPool();
  const ISize size(256, 256);
  auto snapshot = [&](bool* attachments_hit) {
    return DisplayListToTexture(
        ClippedRects(size), size, renderer, /*reset_host_buffer=*/true,
        /*generate_mips=*/false, /*target_pixel_format=*/std::nullopt,
        /*lossless_target=*/false, /*pooled_target=*/true,
        /*pool_hit=*/nullptr, /*snapshot_host_buffers=*/false,
        /*host_buffers_reset=*/nullptr, /*pooled_attachments=*/true,
        attachments_hit);
  };
  auto held = snapshot(nullptr);
  ASSERT_TRUE(held);
  const std::vector<uint8_t> pixels = ReadBack(GetContext(), held);
  ASSERT_TRUE(snapshot(nullptr));
  EXPECT_EQ(pool.targets.size(), 2u);
  EXPECT_EQ(pool.attachments.size(), 1u);

  // Frames with a snapshot now and then: nothing goes.
  for (uint32_t i = 0; i < SnapshotTargetPool::kIdleFrames - 1; i++) {
    content.ResetTransientsBuffers();
  }
  bool hit = false;
  ASSERT_TRUE(snapshot(&hit));
  EXPECT_TRUE(hit);
  for (uint32_t i = 0; i < SnapshotTargetPool::kIdleFrames - 1; i++) {
    content.ResetTransientsBuffers();
  }
  EXPECT_EQ(pool.targets.size(), 2u);
  EXPECT_EQ(pool.attachments.size(), 1u);

  // One more frame: the attachments and the target nothing holds go.
  content.ResetTransientsBuffers();
  EXPECT_TRUE(pool.attachments.empty());
  ASSERT_EQ(pool.targets.size(), 1u);
  EXPECT_EQ(pool.targets.front().texture, held);

  // The next snapshot starts over.
  auto next = snapshot(&hit);
  ASSERT_TRUE(next);
  EXPECT_FALSE(hit);
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), next), pixels), 0);
  EXPECT_EQ(MaxDifference(ReadBack(GetContext(), held), pixels), 0);
}

// P15: an uploaded image whose mipmaps were left unfilled draws exactly like
// one that has them when the draw can only read level 0, and nothing fills
// them.
TEST_P(AiksTest, ReelxDeferredMipmapsStayUnfilledForLevelZeroDraws) {
  AiksContext renderer(GetContext(), nullptr);
  auto source = DisplayListToTexture(Stripes(), {256, 256}, renderer);
  ASSERT_TRUE(source);
  auto stock = Uploaded(GetContext(), source, /*deferred=*/false);
  auto deferred = Uploaded(GetContext(), source, /*deferred=*/true);
  ASSERT_TRUE(stock && deferred);
  EXPECT_FALSE(stock->HasDeferredMipmaps());
  ASSERT_TRUE(deferred->HasDeferredMipmaps());
  EXPECT_EQ(deferred->GetMipCount(), stock->GetMipCount());
  const uint64_t generated = GetDeferredMipmapsGenerated();

  struct Case {
    const char* name;
    int extent;
    flutter::DlImageSampling sampling;
  };
  const Case cases[] = {
      {"1:1, mipmap sampling", 256, flutter::DlImageSampling::kMipmapLinear},
      {"1:1, cubic", 256, flutter::DlImageSampling::kCubic},
      {"magnified, mipmap sampling", 384,
       flutter::DlImageSampling::kMipmapLinear},
      {"minified, linear", 32, flutter::DlImageSampling::kLinear},
      {"minified, nearest", 32, flutter::DlImageSampling::kNearestNeighbor},
  };
  for (const Case& c : cases) {
    const ISize size(c.extent, c.extent);
    auto expected = Drawn(GetContext(), renderer,
                          ImageAt(stock, c.extent, c.sampling), size);
    auto actual = Drawn(GetContext(), renderer,
                        ImageAt(deferred, c.extent, c.sampling), size);
    EXPECT_LE(MaxDifference(expected, actual), 1) << c.name;
    EXPECT_TRUE(deferred->HasDeferredMipmaps()) << c.name;
  }
  // A paint that only fades the image still reads it once.
  flutter::DlPaint faded;
  faded.setAlpha(128);
  EXPECT_LE(MaxDifference(
                Drawn(GetContext(), renderer,
                      ImageAt(stock, 256,
                              flutter::DlImageSampling::kMipmapLinear, &faded),
                      {256, 256}),
                Drawn(GetContext(), renderer,
                      ImageAt(deferred, 256,
                              flutter::DlImageSampling::kMipmapLinear, &faded),
                      {256, 256})),
            1);
  EXPECT_TRUE(deferred->HasDeferredMipmaps());
  EXPECT_EQ(GetDeferredMipmapsGenerated(), generated);
}

// P15: the first draw that can read the mipmaps fills them, once, and draws
// what a stock upload draws.
TEST_P(AiksTest, ReelxDeferredMipmapsFillForMinifiedDraws) {
  AiksContext renderer(GetContext(), nullptr);
  auto source = DisplayListToTexture(Stripes(), {256, 256}, renderer);
  ASSERT_TRUE(source);
  auto stock = Uploaded(GetContext(), source, /*deferred=*/false);
  auto deferred = Uploaded(GetContext(), source, /*deferred=*/true);
  ASSERT_TRUE(stock && deferred);
  const uint64_t generated = GetDeferredMipmapsGenerated();

  auto expected = DrawnSmall(GetContext(), renderer, stock,
                             flutter::DlImageSampling::kMipmapLinear);
  auto actual = DrawnSmall(GetContext(), renderer, deferred,
                           flutter::DlImageSampling::kMipmapLinear);
  EXPECT_LE(MaxDifference(expected, actual), 1);
  EXPECT_FALSE(deferred->HasDeferredMipmaps());
  EXPECT_FALSE(deferred->NeedsMipmapGeneration());
  EXPECT_EQ(GetDeferredMipmapsGenerated(), generated + 1);

  // The mipmaps matter here: without them the stripes alias.
  auto unfiltered = DrawnSmall(GetContext(), renderer, stock,
                               flutter::DlImageSampling::kLinear);
  EXPECT_GT(MaxDifference(expected, unfiltered), 8);

  // Filled once.
  actual = DrawnSmall(GetContext(), renderer, deferred,
                      flutter::DlImageSampling::kMipmapLinear);
  EXPECT_LE(MaxDifference(expected, actual), 1);
  EXPECT_EQ(GetDeferredMipmapsGenerated(), generated + 1);
}

// P15: every draw that isn't a plain, unrotated image rectangle gets the
// mipmaps a stock upload has, whether or not it ends up reading them.
TEST_P(AiksTest, ReelxDeferredMipmapsFillForOtherDraws) {
  AiksContext renderer(GetContext(), nullptr);
  auto source = DisplayListToTexture(Stripes(), {256, 256}, renderer);
  ASSERT_TRUE(source);
  auto stock = Uploaded(GetContext(), source, /*deferred=*/false);
  ASSERT_TRUE(stock);

  using Draw = std::function<sk_sp<flutter::DisplayList>(
      const std::shared_ptr<Texture>&)>;
  struct Case {
    const char* name;
    Draw draw;
  };
  const flutter::DlImageSampling mipmap =
      flutter::DlImageSampling::kMipmapLinear;
  const Case cases[] = {
      {"blurred",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DlPaint paint;
         paint.setImageFilter(flutter::DlImageFilter::MakeBlur(
             12, 12, flutter::DlTileMode::kDecal));
         return ImageAt(image, 256, mipmap, &paint);
       }},
      {"colour filter",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DlPaint paint;
         paint.setColorFilter(flutter::DlColorFilter::MakeBlend(
             flutter::DlColor::kRed(), flutter::DlBlendMode::kMultiply));
         return ImageAt(image, 256, mipmap, &paint);
       }},
      {"advanced blend",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DlPaint paint;
         paint.setBlendMode(flutter::DlBlendMode::kScreen);
         return ImageAt(image, 256, mipmap, &paint);
       }},
      {"rotated",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DisplayListBuilder builder;
         builder.Translate(128, 128);
         builder.Rotate(30);
         builder.Translate(-128, -128);
         builder.DrawImageRect(DlImageImpeller::Make(image),
                               flutter::DlRect::MakeWH(256, 256),
                               flutter::DlRect::MakeWH(256, 256), mipmap);
         return builder.Build();
       }},
      {"scaled down by the canvas",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DisplayListBuilder builder;
         builder.Scale(0.125, 0.125);
         builder.DrawImageRect(DlImageImpeller::Make(image),
                               flutter::DlRect::MakeWH(256, 256),
                               flutter::DlRect::MakeWH(256, 256), mipmap);
         return builder.Build();
       }},
      {"nine patch",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DisplayListBuilder builder;
         builder.DrawImageNine(DlImageImpeller::Make(image),
                               flutter::DlIRect::MakeLTRB(64, 64, 192, 192),
                               flutter::DlRect::MakeWH(96, 96),
                               flutter::DlFilterMode::kLinear);
         return builder.Build();
       }},
      {"atlas",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DisplayListBuilder builder;
         const flutter::DlRSTransform transform =
             flutter::DlRSTransform::Make({0, 0}, 0.125, Radians(0));
         const flutter::DlRect source = flutter::DlRect::MakeWH(256, 256);
         builder.DrawAtlas(DlImageImpeller::Make(image), &transform, &source,
                           /*colors=*/nullptr, /*count=*/1,
                           flutter::DlBlendMode::kSrcOver, mipmap,
                           /*cullRect=*/nullptr);
         return builder.Build();
       }},
      {"image shader",
       [&](const std::shared_ptr<Texture>& image) {
         flutter::DlPaint paint;
         const flutter::DlMatrix matrix =
             flutter::DlMatrix::MakeScale({0.125, 0.125, 1});
         paint.setColorSource(flutter::DlColorSource::MakeImage(
             DlImageImpeller::Make(image), flutter::DlTileMode::kRepeat,
             flutter::DlTileMode::kRepeat, mipmap, &matrix));
         flutter::DisplayListBuilder builder;
         builder.DrawRect(flutter::DlRect::MakeWH(256, 256), paint);
         return builder.Build();
       }},
  };
  for (const Case& c : cases) {
    auto deferred = Uploaded(GetContext(), source, /*deferred=*/true);
    ASSERT_TRUE(deferred) << c.name;
    ASSERT_TRUE(deferred->HasDeferredMipmaps()) << c.name;
    auto expected = Drawn(GetContext(), renderer, c.draw(stock), {256, 256});
    auto actual = Drawn(GetContext(), renderer, c.draw(deferred), {256, 256});
    EXPECT_LE(MaxDifference(expected, actual), 1) << c.name;
    EXPECT_FALSE(deferred->HasDeferredMipmaps()) << c.name;
    EXPECT_FALSE(deferred->NeedsMipmapGeneration()) << c.name;
  }
}

// P15, for P1's snapshots: a snapshot made without mipmaps and drawn at its
// own size with mipmap sampling doesn't get the mipmapped copy either.
TEST_P(AiksTest, ReelxLazySnapshotDrawnAtItsSizeMakesNoCopy) {
  AiksContext renderer(GetContext(), nullptr);
  auto lazy = DisplayListToTexture(Stripes(), {256, 256}, renderer,
                                   /*reset_host_buffer=*/true,
                                   /*generate_mips=*/false);
  ASSERT_TRUE(lazy);
  const std::vector<uint8_t> pixels = ReadBack(GetContext(), lazy);
  lazy->SetWantsLazyMipmaps(true);

  auto drawn = Drawn(
      GetContext(), renderer,
      ImageAt(lazy, 256, flutter::DlImageSampling::kMipmapLinear), {256, 256});
  EXPECT_LE(MaxDifference(drawn, pixels), 1);
  EXPECT_EQ(lazy->GetLazyMipmapped(), nullptr);

  // Minified, it still does.
  DrawnSmall(GetContext(), renderer, lazy,
             flutter::DlImageSampling::kMipmapLinear);
  EXPECT_NE(lazy->GetLazyMipmapped(), nullptr);
}

TEST(ReelxLazyMipmapsTest, ImageRectNeverMinifies) {
  const Rect source = Rect::MakeXYWH(10, 20, 200, 100);
  // The image's own size, larger, and larger through the canvas.
  EXPECT_TRUE(
      ImageRectNeverMinifies(Matrix(), source, Rect::MakeXYWH(5, 5, 200, 100)));
  EXPECT_TRUE(ImageRectNeverMinifies(Matrix(), source, Rect::MakeWH(300, 100)));
  EXPECT_TRUE(ImageRectNeverMinifies(Matrix::MakeScale({2, 2, 1}), source,
                                     Rect::MakeWH(100, 50)));
  EXPECT_TRUE(ImageRectNeverMinifies(
      Matrix::MakeTranslation({7, 9, 0}) * Matrix::MakeScale({-1, 1, 1}),
      source, Rect::MakeWH(200, 100)));
  // Smaller on one axis is smaller.
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix(), source, Rect::MakeWH(300, 99)));
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix::MakeScale({0.5, 1, 1}), source,
                                      Rect::MakeWH(200, 100)));
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix::MakeScale({0.5, 0.5, 1}), source,
                                      Rect::MakeWH(390, 190)));
  // Anything but a scale and a translation counts as smaller.
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix::MakeRotationZ(Radians(0.5)),
                                      source, Rect::MakeWH(400, 400)));
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix::MakeSkew(0.2, 0), source,
                                      Rect::MakeWH(400, 400)));
  Matrix perspective;
  perspective.m[11] = 0.001;
  EXPECT_FALSE(
      ImageRectNeverMinifies(perspective, source, Rect::MakeWH(400, 400)));
  // Nothing to draw.
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix(), Rect(), Rect::MakeWH(4, 4)));
  EXPECT_FALSE(ImageRectNeverMinifies(Matrix(), source, Rect()));
}

}  // namespace testing
}  // namespace impeller
