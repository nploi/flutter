// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/lazy_mipmaps.h"

#include <atomic>
#include <cmath>

#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/context.h"

namespace impeller {

namespace {
std::atomic<uint64_t> g_deferred_mipmaps_generated = 0;
}  // namespace

std::shared_ptr<Texture> TextureForSampling(
    const ContentContext& renderer,
    const std::shared_ptr<Texture>& texture,
    const SamplerDescriptor& sampler) {
  if (!texture || sampler.mip_filter != MipFilter::kLinear ||
      !texture->WantsLazyMipmaps()) {
    return texture;
  }
  if (const auto& mipmapped = texture->GetLazyMipmapped()) {
    return mipmapped;
  }

  const std::shared_ptr<Context>& context = renderer.GetContext();
  TextureDescriptor desc = texture->GetTextureDescriptor();
  desc.mip_count = desc.size.MipCount();
  std::shared_ptr<Texture> mipmapped =
      context->GetResourceAllocator()->CreateTexture(desc);
  if (!mipmapped) {
    return texture;
  }
  mipmapped->SetLabel("Lazy Mipmapped Snapshot");

  std::shared_ptr<CommandBuffer> command_buffer =
      context->CreateCommandBuffer();
  if (!command_buffer) {
    return texture;
  }
  std::shared_ptr<BlitPass> blit_pass = command_buffer->CreateBlitPass();
  if (!blit_pass || !blit_pass->AddCopy(texture, mipmapped) ||
      !blit_pass->GenerateMipmap(mipmapped) || !blit_pass->EncodeCommands()) {
    return texture;
  }
  // Submitted now, ahead of the command buffer of the draw that needs it.
  if (!context->GetCommandQueue()->Submit({command_buffer}).ok()) {
    return texture;
  }
  texture->SetLazyMipmapped(mipmapped);
  return mipmapped;
}

bool GenerateDeferredMipmaps(const std::shared_ptr<Context>& context,
                             const std::shared_ptr<Texture>& texture) {
  if (!texture || !texture->HasDeferredMipmaps()) {
    return true;
  }
  if (!context) {
    return false;
  }
  std::shared_ptr<CommandBuffer> command_buffer =
      context->CreateCommandBuffer();
  if (!command_buffer) {
    return false;
  }
  command_buffer->SetLabel("Deferred Mipmap Command Buffer");
  std::shared_ptr<BlitPass> blit_pass = command_buffer->CreateBlitPass();
  if (!blit_pass || !blit_pass->GenerateMipmap(texture) ||
      !blit_pass->EncodeCommands()) {
    return false;
  }
  // Submitted now, ahead of the command buffer of the draw that needs it.
  if (!context->GetCommandQueue()->Submit({command_buffer}).ok()) {
    return false;
  }
  // The backend marks the mipmaps as generated when it encodes the command,
  // which OpenGL ES may do later than this.
  texture->SetDeferredMipmaps(false);
  g_deferred_mipmaps_generated.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint64_t GetDeferredMipmapsGenerated() {
  return g_deferred_mipmaps_generated.load(std::memory_order_relaxed);
}

bool ImageRectNeverMinifies(const Matrix& transform,
                            const Rect& source,
                            const Rect& dest) {
  if (!transform.IsTranslationScaleOnly() || source.IsEmpty() ||
      dest.IsEmpty()) {
    return false;
  }
  const Scalar width = std::abs(transform.m[0]) * dest.GetWidth();
  const Scalar height = std::abs(transform.m[5]) * dest.GetHeight();
  return width >= source.GetWidth() && height >= source.GetHeight();
}

}  // namespace impeller
