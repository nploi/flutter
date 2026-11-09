// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_LAZY_MIPMAPS_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_LAZY_MIPMAPS_H_

#include <memory>

#include "impeller/core/sampler_descriptor.h"
#include "impeller/core/texture.h"

namespace impeller {

class ContentContext;

/// Reelx P1: the texture a draw sampling [texture] with [sampler] should
/// use. A texture that wants lazy mipmaps (Texture::WantsLazyMipmaps), drawn
/// with a sampler that reads mipmaps, gets a mipmapped copy made once and
/// kept on the texture; every other draw gets [texture] itself. When the copy
/// can't be made the draw uses [texture], which samples its one level.
std::shared_ptr<Texture> TextureForSampling(
    const ContentContext& renderer,
    const std::shared_ptr<Texture>& texture,
    const SamplerDescriptor& sampler);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_LAZY_MIPMAPS_H_
