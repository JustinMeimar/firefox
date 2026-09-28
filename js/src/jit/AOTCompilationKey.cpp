/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOTCompilationKey.h"

#include "vm/JSScript.h"
#include "vm/Scope.h"
#include "wasm/WasmCompile.h"

#include "vm/JSScript-inl.h"

namespace js::jit {

void AOTCompilationKey::append(mozilla::Span<const uint8_t> value) {
  if (!valid_) {
    return;
  }
  if (comparing_) {
    valid_ = value.size() <= expected_.size() - cursor_ &&
             (value.empty() || memcmp(expected_.data() + cursor_, value.data(),
                                      value.size()) == 0);
    if (valid_) {
      cursor_ += value.size();
    }
  } else {
    valid_ = data_.append(value.data(), value.size());
  }
}

void AOTCompilationKey::scalar(uint32_t value) {
  uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                      uint8_t(value >> 24)};
  append(bytes);
}

void AOTCompilationKey::bytes(mozilla::Span<const uint8_t> value) {
  if (value.size() > UINT32_MAX) {
    valid_ = false;
    return;
  }
  scalar(uint32_t(value.size()));
  append(value);
  const uint8_t padding[3] = {};
  append({padding, (4 - value.size() % 4) % 4});
}

}  // namespace js::jit

#include "jit/AOTCompilationKeyGenerated.inc"
