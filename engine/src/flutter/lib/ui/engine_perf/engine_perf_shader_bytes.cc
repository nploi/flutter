// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/engine_perf/engine_perf_shader_bytes.h"

#include <map>
#include <mutex>
#include <vector>

#include "flutter/impeller/runtime_stage/runtime_stage.h"
#include "third_party/tonic/converter/dart_converter.h"

namespace flutter {
namespace engine_perf {

namespace {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

std::mutex& StoreMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, Bytes>& Store() {
  static std::map<std::string, Bytes> store;
  return store;
}

bool IsShaderBundle(const Bytes& bytes) {
  auto mapping = std::make_shared<fml::NonOwnedMapping>(
      bytes->data(), bytes->size(),
      [keep = Bytes(bytes)](const uint8_t*, size_t) mutable { keep.reset(); });
  auto stages = impeller::RuntimeStage::DecodeRuntimeStages(mapping);
  if (!stages.ok()) {
    return false;
  }
  for (const auto& stage : *stages) {
    if (stage.second) {
      return true;
    }
  }
  return false;
}

bool ReadKey(Dart_Handle handle, std::string* key) {
  if (!Dart_IsString(handle)) {
    return false;
  }
  *key = tonic::StdStringFromDart(handle);
  return !key->empty();
}

}  // namespace

int32_t RegisterShaderBytes(const std::string& key,
                            const uint8_t* data,
                            size_t length) {
  if (key.empty() || data == nullptr || length == 0) {
    return kShaderBytesBadArgument;
  }
  Bytes bytes =
      std::make_shared<const std::vector<uint8_t>>(data, data + length);
  if (!IsShaderBundle(bytes)) {
    return kShaderBytesNotABundle;
  }
  std::scoped_lock lock(StoreMutex());
  Store()[key] = std::move(bytes);
  return kShaderBytesOk;
}

int32_t UnregisterShaderBytes(const std::string& key) {
  std::scoped_lock lock(StoreMutex());
  return Store().erase(key) == 1 ? kShaderBytesOk : kShaderBytesNotRegistered;
}

std::unique_ptr<fml::Mapping> GetRegisteredShader(const std::string& key) {
  Bytes bytes;
  {
    std::scoped_lock lock(StoreMutex());
    auto found = Store().find(key);
    if (found == Store().end()) {
      return nullptr;
    }
    bytes = found->second;
  }
  const uint8_t* data = bytes->data();
  size_t size = bytes->size();
  return std::make_unique<fml::NonOwnedMapping>(
      data, size, [bytes](const uint8_t*, size_t) mutable { bytes.reset(); });
}

void ClearShaderBytesForTesting() {
  std::scoped_lock lock(StoreMutex());
  Store().clear();
}

}  // namespace engine_perf
}  // namespace flutter

extern "C" {

int32_t EnginePerfRegisterShaderBytes(Dart_Handle key, Dart_Handle bytes) {
  std::string name;
  if (!flutter::engine_perf::ReadKey(key, &name) || !Dart_IsTypedData(bytes) ||
      Dart_GetTypeOfTypedData(bytes) != Dart_TypedData_kUint8) {
    return flutter::engine_perf::kShaderBytesBadArgument;
  }
  Dart_TypedData_Type type;
  void* data = nullptr;
  intptr_t length = 0;
  if (Dart_IsError(Dart_TypedDataAcquireData(bytes, &type, &data, &length))) {
    return flutter::engine_perf::kShaderBytesBadArgument;
  }
  // RegisterShaderBytes copies the bytes and makes no Dart API call, so it
  // runs while the list is held.
  int32_t result = flutter::engine_perf::RegisterShaderBytes(
      name, static_cast<const uint8_t*>(data), static_cast<size_t>(length));
  Dart_TypedDataReleaseData(bytes);
  return result;
}

int32_t EnginePerfUnregisterShaderBytes(Dart_Handle key) {
  std::string name;
  if (!flutter::engine_perf::ReadKey(key, &name)) {
    return flutter::engine_perf::kShaderBytesBadArgument;
  }
  return flutter::engine_perf::UnregisterShaderBytes(name);
}

}  // extern "C"
