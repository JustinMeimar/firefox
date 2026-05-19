/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTInstrumentation_h
#define jit_AOTInstrumentation_h

#include "mozilla/Likely.h"
#ifndef JS_STANDALONE
#  include "mozilla/ProcessType.h"
#endif
#include "mozilla/TimeStamp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "vm/MutexIDs.h"

namespace js::jit {

enum AOTInstrCh : uint32_t {
  AOTInstr_IC        = 1 << 0,
  AOTInstr_Lifecycle = 1 << 1,
  AOTInstr_Timing    = 1 << 2,
  AOTInstr_BLInterp  = 1 << 3,
  AOTInstr_All       = 0xFFFFFFFF,
};

struct AOTInstrumentation {
  uint32_t channels = 0;
  FILE* out = nullptr;
  mozilla::TimeStamp epoch;
  const char* procTag = "parent";
  js::Mutex lock MOZ_UNANNOTATED;

  AOTInstrumentation() : lock(mutexid::AOTInstrumentation) {}

  void init() {
    const char* env = getenv("JS_AOT_INSTR");
    if (!env) return;
    epoch = mozilla::TimeStamp::Now();
    const char* file = getenv("JS_AOT_INSTR_FILE");
    if (file && *file) {
      char buf[2048];
      snprintf(buf, sizeof(buf), "%s.%d", file, int(getpid()));
      out = fopen(buf, "w");
      if (out) setbuf(out, nullptr);
    }
    if (!out) out = stderr;
    if (strcmp(env, "1") == 0 || strcmp(env, "all") == 0) {
      channels = AOTInstr_All;
    } else {
      channels = 0;
      if (strstr(env, "ic")) channels |= AOTInstr_IC;
      if (strstr(env, "lifecycle")) channels |= AOTInstr_Lifecycle;
      if (strstr(env, "timing")) channels |= AOTInstr_Timing;
      if (strstr(env, "blinterp")) channels |= AOTInstr_BLInterp;
      if (!channels) channels = AOTInstr_All;
    }
#ifndef JS_STANDALONE
    if (mozilla::GetGeckoProcessType() == GeckoProcessType_Content) {
      procTag = "content";
    }
#endif
  }

  bool enabled(uint32_t ch) const { return (channels & ch) != 0; }

  double elapsedUs() const {
    return (mozilla::TimeStamp::Now() - epoch).ToMicroseconds();
  }

  void close() {
    if (out && out != stderr && out != stdout) {
      fclose(out);
    }
    out = nullptr;
    channels = 0;
  }
};

inline AOTInstrumentation gAOTInstr;

#define AOT_INSTR(ch, fmt, ...)                                  \
  do {                                                           \
    if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled(ch))) {       \
      js::LockGuard<js::Mutex> _aotLock(                         \
          ::js::jit::gAOTInstr.lock);                            \
      fprintf(::js::jit::gAOTInstr.out, "ts=%.0f " fmt,         \
              ::js::jit::gAOTInstr.elapsedUs(),                  \
              ##__VA_ARGS__);                                    \
    }                                                            \
  } while (0)

#define AOT_TIMER_BEGIN(label)                                   \
  mozilla::TimeStamp aotTimer_##label;                           \
  if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled(                \
          ::js::jit::AOTInstr_Timing))) {                        \
    aotTimer_##label = mozilla::TimeStamp::Now();                \
  }

#define AOT_TIMER_END(label, event, component, extraFmt, ...)    \
  do {                                                           \
    if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled(              \
            ::js::jit::AOTInstr_Timing)) &&                      \
        !aotTimer_##label.IsNull()) {                            \
      auto elapsed_ = mozilla::TimeStamp::Now() - aotTimer_##label; \
      double us_ = elapsed_.ToMicroseconds();                    \
      js::LockGuard<js::Mutex> _aotLock(                         \
          ::js::jit::gAOTInstr.lock);                            \
      fprintf(::js::jit::gAOTInstr.out,                          \
              "ts=%.0f %s component=%s us=%.0f" extraFmt "\n",  \
              ::js::jit::gAOTInstr.elapsedUs(),                  \
              event, component, us_, ##__VA_ARGS__);             \
    }                                                            \
  } while (0)

}  // namespace js::jit

#endif  // jit_AOTInstrumentation_h
