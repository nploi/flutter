// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/render_target.h"
#include "vulkan/vulkan_enums.hpp"

namespace impeller {
namespace testing {

TEST(RenderPassVK, DoesNotRedundantlySetStencil) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  std::shared_ptr<Context> copy = context;
  auto cmd_buffer = context->CreateCommandBuffer();

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreenMSAA(*copy.get(), {1, 1}, 1);

  std::shared_ptr<RenderPass> render_pass =
      cmd_buffer->CreateRenderPass(target);

  // Stencil reference set once at buffer start.
  auto called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            1);

  // Duplicate stencil ref is not replaced.
  render_pass->SetStencilReference(0);
  render_pass->SetStencilReference(0);
  render_pass->SetStencilReference(0);

  called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            1);

  // Different stencil value is updated.
  render_pass->SetStencilReference(1);
  called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            2);
}

// Regression guard for the bug where `RenderPassVK::SetViewport` silently
// dropped the user's X and Y offsets and the depth range, only honoring
// width and height.
TEST(RenderPassVK, SetViewportPropagatesAllUserSuppliedFields) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  auto cmd_buffer = context->CreateCommandBuffer();

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreenMSAA(*context, {100, 100},
                                                      /*mip_count=*/1);

  std::shared_ptr<RenderPass> render_pass =
      cmd_buffer->CreateRenderPass(target);

  // The render pass constructor sets an initial full-target viewport. Capture
  // a reference to the recorded viewport list before the user-driven call so
  // we can isolate the call under test.
  VkCommandBuffer raw_cmd_buffer =
      CommandBufferVK::Cast(*cmd_buffer).GetCommandBuffer();
  const std::vector<VkViewport>& recorded =
      GetRecordedViewports(raw_cmd_buffer);
  ASSERT_EQ(recorded.size(), 1u);  // Initial full-target viewport.

  render_pass->SetViewport(Viewport{
      .rect = Rect::MakeXYWH(25, 10, 50, 80),
      .depth_range = DepthRange{.z_near = 0.25f, .z_far = 0.75f},
  });

  ASSERT_EQ(recorded.size(), 2u);
  const VkViewport& vp = recorded[1];
  EXPECT_FLOAT_EQ(vp.x, 25.0f);
  // Y is computed for the negative-height Y-flip: `y + height` of the
  // original rect.
  EXPECT_FLOAT_EQ(vp.y, 90.0f);
  EXPECT_FLOAT_EQ(vp.width, 50.0f);
  EXPECT_FLOAT_EQ(vp.height, -80.0f);
  EXPECT_FLOAT_EQ(vp.minDepth, 0.25f);
  EXPECT_FLOAT_EQ(vp.maxDepth, 0.75f);
}

// Reelx P2a: a resolve texture reused with new MSAA and stencil textures (the
// snapshot target pool) must not keep the framebuffer cached for the old
// ones, whose image views are gone.
TEST(RenderPassVK, ForgottenRenderTargetCachesMakeANewFramebuffer) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());

  RenderTarget first = allocator.CreateOffscreenMSAA(*context, {4, 4}, 1);
  std::shared_ptr<Texture> resolve =
      first.GetColorAttachment(0).resolve_texture;
  {
    auto cmd_buffer = context->CreateCommandBuffer();
    auto render_pass = cmd_buffer->CreateRenderPass(first);
    ASSERT_TRUE(render_pass);
  }
  TextureVK& resolve_vk = TextureVK::Cast(*resolve);
  auto old_framebuffer =
      resolve_vk.GetCachedFrameData(SampleCount::kCount4).framebuffer;
  ASSERT_NE(old_framebuffer, nullptr);

  resolve->ForgetRenderTargetCaches();
  EXPECT_EQ(resolve_vk.GetCachedFrameData(SampleCount::kCount4).framebuffer,
            nullptr);
  EXPECT_EQ(resolve_vk.GetCachedFrameData(SampleCount::kCount4).render_pass,
            nullptr);

  // A new render target around the same resolve texture gets its own
  // framebuffer.
  RenderTarget second = allocator.CreateOffscreenMSAA(
      *context, {4, 4}, 1, "Second",
      RenderTarget::kDefaultColorAttachmentConfigMSAA,
      RenderTarget::kDefaultStencilAttachmentConfig,
      /*existing_color_msaa_texture=*/nullptr,
      /*existing_color_resolve_texture=*/resolve);
  {
    auto cmd_buffer = context->CreateCommandBuffer();
    auto render_pass = cmd_buffer->CreateRenderPass(second);
    ASSERT_TRUE(render_pass);
  }
  auto new_framebuffer =
      resolve_vk.GetCachedFrameData(SampleCount::kCount4).framebuffer;
  ASSERT_NE(new_framebuffer, nullptr);
  EXPECT_NE(new_framebuffer, old_framebuffer);
}

}  // namespace testing
}  // namespace impeller
