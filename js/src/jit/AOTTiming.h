/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTTiming_h
#define jit_AOTTiming_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Attributes.h"
#  include "mozilla/TimeStamp.h"

#  include <cstdint>

namespace js::jit {

enum class AOTTimingPhase : uint8_t {
  ImageCompatibility,
  InterpreterAttach,
  RITInitialization,
  ICCorpusAttach,
  BaselineFunctionLookup,
  BaselineFunctionReconstruct,
  ICImageLookup,
  ICPrivateAttach,
  RuntimeBaselineCompile,
  RuntimeICCompile,
  Limit,
};

enum class AOTTimingCounter : uint8_t {
  InterpreterCodeBytes,
  InterpreterMetadataBytes,
  InterpreterWrappers,
  ICCorpusAttempted,
  ICCorpusLoaded,
  ICCorpusCodeBytes,
  ICCorpusMetadataBytes,
  ICCorpusWrappers,
  BaselineLookupHits,
  BaselineLookupMisses,
  BaselineCodeBytes,
  BaselineMetadataBytes,
  BaselineWrappers,
  ICImageLookupHits,
  ICImageLookupMisses,
  ICPrivateStubs,
  ICPrivateStubBytes,
  Limit,
};

class AOTTiming {
 public:
  static bool IsEnabled();
  static void Record(AOTTimingPhase phase, mozilla::TimeDuration duration);
  static void AddCounter(AOTTimingCounter counter, uint64_t value = 1);
  static void FlushAndWrite(const char* processType);
};

class MOZ_RAII AutoAOTTimer {
 public:
  explicit AutoAOTTimer(AOTTimingPhase phase, bool condition = true);
  ~AutoAOTTimer();

  void Stop();

  AutoAOTTimer(const AutoAOTTimer&) = delete;
  AutoAOTTimer& operator=(const AutoAOTTimer&) = delete;

 private:
  AOTTimingPhase phase_;
  mozilla::TimeStamp start_;
  bool enabled_;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTTiming_h
