// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_LAZY_MIPMAPS_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_LAZY_MIPMAPS_H_

#include <cstdint>
#include <memory>

#include "impeller/core/sampler_descriptor.h"
#include "impeller/core/texture.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/rect.h"

namespace impeller {

class ContentContext;
class Context;

/// EnginePerf P1: the texture a draw sampling [texture] with [sampler] should
/// use. A texture that wants lazy mipmaps (Texture::WantsLazyMipmaps), drawn
/// with a sampler that reads mipmaps, gets a mipmapped copy made once and
/// kept on the texture; every other draw gets [texture] itself. When the copy
/// can't be made the draw uses [texture], which samples its one level.
std::shared_ptr<Texture> TextureForSampling(
    const ContentContext& renderer,
    const std::shared_ptr<Texture>& texture,
    const SamplerDescriptor& sampler);

/// EnginePerf P15: fills the levels above 0 of a texture whose upload left them
/// out (Texture::HasDeferredMipmaps), in a command buffer submitted now,
/// ahead of the draw that reads them. Returns whether the texture's mipmaps
/// can be sampled; when it can't fill them, only level 0 can.
bool GenerateDeferredMipmaps(const std::shared_ptr<Context>& context,
                             const std::shared_ptr<Texture>& texture);

/// How many times GenerateDeferredMipmaps filled a texture's mipmaps.
uint64_t GetDeferredMipmapsGenerated();

/// EnginePerf P15: whether drawing the texels of [source] onto [dest] under
/// [transform] gives every texel at least a pixel in both directions, so
/// that a mipmap sampler reads level 0 and nothing else. Only answered for
/// transforms that scale and translate; any other is taken to minify.
bool ImageRectNeverMinifies(const Matrix& transform,
                            const Rect& source,
                            const Rect& dest);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_LAZY_MIPMAPS_H_
