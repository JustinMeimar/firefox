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

// Three artifact classes, two events each. A cell that runs without AOT
// records only the compile phases; a cell with AOT installed records the
// install phases and any residual compile work for artifacts missing from
// the image.
enum class AOTTimingPhase : uint8_t {
  InterpreterInstall,
  InterpreterGenerate,
  BaselineInstall,
  BaselineCompile,
  ICInstall,
  ICCompile,
  Limit,
};

// Bytes contributed by the AOT image per artifact class (code + metadata).
// Runtime cells leave these at zero.
enum class AOTTimingCounter : uint8_t {
  InterpreterImageBytes,
  BaselineImageBytes,
  ICImageBytes,
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
