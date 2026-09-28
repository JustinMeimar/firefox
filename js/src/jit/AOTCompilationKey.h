/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTCompilationKey_h
#define jit_AOTCompilationKey_h

#ifdef ENABLE_JS_AOT

#  include "jit/AOTImage.h"
#  include "jit/JitOptions.h"

class JSScript;

namespace js::jit {

class AOTCompilationKey {
 public:
  AOTCompilationKey() = default;
  explicit AOTCompilationKey(mozilla::Span<const uint8_t> expected)
      : expected_(expected), comparing_(true) {}

  void scalar(uint32_t value);
  void bytes(mozilla::Span<const uint8_t> value);
  bool complete() const {
    return valid_ && (!comparing_ || cursor_ == expected_.size());
  }
  mozilla::Span<const uint8_t> data() const {
    return {data_.begin(), data_.length()};
  }

 private:
  void append(mozilla::Span<const uint8_t> value);

  Vector<uint8_t, 0, SystemAllocPolicy> data_;
  mozilla::Span<const uint8_t> expected_;
  size_t cursor_ = 0;
  bool comparing_ = false;
  bool valid_ = true;
};

uint32_t AOTICContext();

void WriteAOTContext(AOTCompilationKey& key, AOTBlobKind kind,
                     const DefaultJitOptions& options, bool profiling);
void WriteAOTBaselineInputs(AOTCompilationKey& key, JSScript* script,
                            uint32_t baseWarmUpThreshold, bool ionCompileable,
                            bool debugInstrumentation);
void WriteAOTICInputs(AOTCompilationKey& key, const AOTICStubMetadata& md);

}  // namespace js::jit

#endif
#endif
