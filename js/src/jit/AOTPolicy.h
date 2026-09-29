/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTPolicy_h
#define jit_AOTPolicy_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/EnumSet.h"

#  include "jit/AOTImageFormatGenerated.h"
#  include "js/UniquePtr.h"

struct JSContext;

namespace js::jit {

struct DefaultJitOptions;

enum class AOTMissBehavior { Compile, StayInTier, Fail };

class AOTPolicy {
  mozilla::EnumSet<AOTBlobKind> load_;
  mozilla::EnumSet<AOTBlobKind> capture_;
  AOTMissBehavior missBehavior_ = AOTMissBehavior::Compile;
  JS::UniqueChars recordDirectory_;
  bool recordSelfHosted_ = false;

 public:
  [[nodiscard]] bool init(JSContext* cx, const DefaultJitOptions& options);

  bool shouldLoad(AOTBlobKind kind) const { return load_.contains(kind); }
  bool shouldCapture(AOTBlobKind kind) const { return capture_.contains(kind); }
  bool enabled() const { return !load_.isEmpty() || !capture_.isEmpty(); }
  AOTMissBehavior missBehavior(AOTBlobKind kind) const {
    if (kind == AOTBlobKind::BaselineInterpreter) {
      return AOTMissBehavior::Compile;
    }
    return missBehavior_;
  }
  bool allowsCompilation() const {
    return missBehavior_ != AOTMissBehavior::StayInTier;
  }
  const char* recordDirectory() const { return recordDirectory_.get(); }
  bool recordSelfHosted() const { return recordSelfHosted_; }
};

}  // namespace js::jit

#endif
#endif
