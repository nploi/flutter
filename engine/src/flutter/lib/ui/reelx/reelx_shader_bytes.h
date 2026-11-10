// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_REELX_REELX_SHADER_BYTES_H_
#define FLUTTER_LIB_UI_REELX_REELX_SHADER_BYTES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "flutter/fml/mapping.h"
#include "flutter/lib/ui/reelx/reelx_ffi.h"

// Fragment shaders from bytes.
//
// FragmentProgram.fromAsset(key) looks [key] up here before the engine's
// asset manager, so a shader bundle registered from Dart loads with the
// stock dart:ui API and no bundled asset. The bytes are an impellerc bundle
// (`impellerc --iplr`, the same file the flutter tool builds from a .frag in
// pubspec's shaders list), compiled for the backends the app runs on.

extern "C" {

/// Registers [bytes] (a Uint8List) under [key] (a String) for the whole
/// process, replacing bytes registered before under the same key. Returns 0,
/// -1 when an argument isn't a non-empty String / Uint8List, -3 when the
/// bytes aren't a shader bundle. dart:ui keeps a program it loaded for a key,
/// so give changed bytes a new key (a content hash, for example).
REELX_EXPORT int32_t ReelxRegisterShaderBytes(Dart_Handle key,
                                              Dart_Handle bytes);

/// Forgets the bytes registered under [key] (programs already loaded keep
/// working). Returns 0, -1 for a bad key, -4 when nothing was registered.
REELX_EXPORT int32_t ReelxUnregisterShaderBytes(Dart_Handle key);

}  // extern "C"

namespace flutter {
namespace reelx {

/// Result codes of the functions above.
enum ShaderBytesResult : int32_t {
  kShaderBytesOk = 0,
  kShaderBytesBadArgument = -1,
  kShaderBytesNotABundle = -3,
  kShaderBytesNotRegistered = -4,
};

int32_t RegisterShaderBytes(const std::string& key,
                            const uint8_t* data,
                            size_t length);

int32_t UnregisterShaderBytes(const std::string& key);

/// The bytes registered under [key], or null. The mapping keeps them alive
/// after they are unregistered or replaced.
std::unique_ptr<fml::Mapping> GetRegisteredShader(const std::string& key);

/// Test hook.
void ClearShaderBytesForTesting();

}  // namespace reelx
}  // namespace flutter

#endif  // FLUTTER_LIB_UI_REELX_REELX_SHADER_BYTES_H_
