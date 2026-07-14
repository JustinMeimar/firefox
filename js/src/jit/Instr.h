/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Instr_h
#define jit_Instr_h

#include "mozilla/Likely.h"
#ifndef JS_STANDALONE
#  include "mozilla/ProcessType.h"
#endif
#include "mozilla/TimeStamp.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "vm/MutexIDs.h"

// Research instrumentation for the FrostMonkey phase-2 preliminary
// evaluations; not intended for production. Env vars read at init():
//
//   JS_INSTR       "1" or "all" enables every channel. Otherwise a
//                  substring match against the channel names
//                  {ic, lifecycle, timing, blinterp, baseline}; a
//                  match that resolves to no channels falls back to
//                  all. Unset leaves instrumentation disabled and
//                  emission sites are a single predicted-taken branch.
//
//   JS_INSTR_FILE  Output path; ".$PID" is appended. Falls back to
//                  stderr if unset or fopen fails.

namespace js::jit {

enum JSInstrCh : uint32_t {
  JSInstr_IC = 1 << 0,
  JSInstr_Lifecycle = 1 << 1,
  JSInstr_Timing = 1 << 2,
  JSInstr_BLInterp = 1 << 3,
  JSInstr_Baseline = 1 << 4,
  JSInstr_All = 0xFFFFFFFF,
};

struct JSInstrumentation {
  uint32_t channels = 0;
  FILE* out = nullptr;
  mozilla::TimeStamp epoch;
  const char* procTag = "parent";
  js::Mutex lock MOZ_UNANNOTATED;

  JSInstrumentation() : lock(mutexid::JSInstrumentation) {}

  void init() {
    const char* env = getenv("JS_INSTR");
    if (!env) return;
    epoch = mozilla::TimeStamp::Now();
    const char* file = getenv("JS_INSTR_FILE");
    if (file && *file) {
      char buf[2048];
      snprintf(buf, sizeof(buf), "%s.%d", file, int(getpid()));
      out = fopen(buf, "w");
      if (out) setbuf(out, nullptr);
    }
    if (!out) out = stderr;
    if (strcmp(env, "1") == 0 || strcmp(env, "all") == 0) {
      channels = JSInstr_All;
    } else {
      channels = 0;
      if (strstr(env, "ic")) channels |= JSInstr_IC;
      if (strstr(env, "lifecycle")) channels |= JSInstr_Lifecycle;
      if (strstr(env, "timing")) channels |= JSInstr_Timing;
      if (strstr(env, "blinterp")) channels |= JSInstr_BLInterp;
      if (strstr(env, "baseline")) channels |= JSInstr_Baseline;
      if (!channels) channels = JSInstr_All;
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

inline JSInstrumentation gJSInstr;

#define JS_INSTR(ch, fmt, ...)                                       \
  do {                                                               \
    if (MOZ_UNLIKELY(::js::jit::gJSInstr.enabled(ch))) {             \
      js::LockGuard<js::Mutex> _jsInstrLock(::js::jit::gJSInstr.lock); \
      fprintf(::js::jit::gJSInstr.out, "ts=%.0f " fmt,               \
              ::js::jit::gJSInstr.elapsedUs(), ##__VA_ARGS__);       \
    }                                                                \
  } while (0)

#define JS_INSTR_TIMER_BEGIN(label)                                  \
  mozilla::TimeStamp jsInstrTimer_##label;                           \
  if (MOZ_UNLIKELY(                                                  \
          ::js::jit::gJSInstr.enabled(::js::jit::JSInstr_Timing))) { \
    jsInstrTimer_##label = mozilla::TimeStamp::Now();                \
  }

#define JS_INSTR_TIMER_END(label, event, component, extraFmt, ...)     \
  do {                                                                 \
    if (MOZ_UNLIKELY(                                                  \
            ::js::jit::gJSInstr.enabled(::js::jit::JSInstr_Timing)) && \
        !jsInstrTimer_##label.IsNull()) {                              \
      auto elapsed_ = mozilla::TimeStamp::Now() - jsInstrTimer_##label; \
      double us_ = elapsed_.ToMicroseconds();                          \
      js::LockGuard<js::Mutex> _jsInstrLock(::js::jit::gJSInstr.lock); \
      fprintf(::js::jit::gJSInstr.out,                                 \
              "ts=%.0f %s component=%s us=%.0f" extraFmt "\n",         \
              ::js::jit::gJSInstr.elapsedUs(), event, component, us_,  \
              ##__VA_ARGS__);                                          \
    }                                                                  \
  } while (0)

}  // namespace js::jit

#endif  // jit_Instr_h
