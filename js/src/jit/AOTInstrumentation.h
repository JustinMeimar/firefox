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
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "vm/MutexIDs.h"

// Research instrumentation for the FrostMonkey paper; not intended for
// production. Env vars read at init():
//
//   JS_AOT_INSTR      "1" or "all" enables every channel. Otherwise a
//                     substring match against the channel names
//                     {ic, lifecycle, timing, blinterp}; a match that
//                     resolves to no channels falls back to all. Unset
//                     leaves instrumentation disabled.
//
//   JS_AOT_INSTR_FILE Output path; ".$PID" is appended. Falls back to
//                     stderr if unset or fopen fails.
//
//   JS_AOT_PGO_DIR    Directory for IC stub dumps. Required when the ic
//                     channel is on; IC dumps are disabled with a
//                     warning if unset or mkdir fails.

namespace js::jit {

enum AOTInstrCh : uint32_t {
  AOTInstr_IC        = 1 << 0,
  AOTInstr_Lifecycle = 1 << 1,
  AOTInstr_Timing    = 1 << 2,
  AOTInstr_BLInterp  = 1 << 3,
  AOTInstr_Baseline  = 1 << 4,
  AOTInstr_All       = 0xFFFFFFFF,
};

struct AOTInstrumentation {
  uint32_t channels = 0;
  FILE* out = nullptr;
  mozilla::TimeStamp epoch;
  const char* procTag = "parent";
  const char* pgoDumpDir = nullptr;
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
      if (strstr(env, "baseline")) channels |= AOTInstr_Baseline;
      if (!channels) channels = AOTInstr_All;
    }
    if (channels & (AOTInstr_IC | AOTInstr_Baseline)) {
      pgoDumpDir = getenv("JS_AOT_PGO_DIR");
      if (pgoDumpDir && *pgoDumpDir) {
        if (mkdir(pgoDumpDir, 0755) != 0 && errno != EEXIST) {
          fprintf(stderr,
                  "AOT: mkdir %s (JS_AOT_PGO_DIR) failed: %s -- PGO "
                  "dumps disabled.\n",
                  pgoDumpDir, strerror(errno));
          pgoDumpDir = nullptr;
        }
      } else {
        pgoDumpDir = nullptr;
        fprintf(stderr,
                "AOT: PGO channel enabled but JS_AOT_PGO_DIR is unset -- "
                "PGO dumps disabled.\n");
      }
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

#ifdef ENABLE_JS_AOT
#  define AOT_INSTR(ch, fmt, ...)                                \
    do {                                                         \
      if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled(ch))) {     \
        js::LockGuard<js::Mutex> _aotLock(                       \
            ::js::jit::gAOTInstr.lock);                          \
        fprintf(::js::jit::gAOTInstr.out, "ts=%.0f " fmt,       \
                ::js::jit::gAOTInstr.elapsedUs(),                \
                ##__VA_ARGS__);                                  \
      }                                                          \
    } while (0)

#  define AOT_TIMER_BEGIN(label)                                 \
    mozilla::TimeStamp aotTimer_##label;                         \
    if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled(              \
            ::js::jit::AOTInstr_Timing))) {                      \
      aotTimer_##label = mozilla::TimeStamp::Now();              \
    }

#  define AOT_TIMER_END(label, event, component, extraFmt, ...)  \
    do {                                                         \
      if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled(            \
              ::js::jit::AOTInstr_Timing)) &&                    \
          !aotTimer_##label.IsNull()) {                          \
        auto elapsed_ = mozilla::TimeStamp::Now() - aotTimer_##label; \
        double us_ = elapsed_.ToMicroseconds();                  \
        js::LockGuard<js::Mutex> _aotLock(                       \
            ::js::jit::gAOTInstr.lock);                          \
        fprintf(::js::jit::gAOTInstr.out,                        \
                "ts=%.0f %s component=%s us=%.0f" extraFmt "\n",\
                ::js::jit::gAOTInstr.elapsedUs(),                \
                event, component, us_, ##__VA_ARGS__);           \
      }                                                          \
    } while (0)
#else
#  define AOT_INSTR(ch, fmt, ...) do { } while (0)
#  define AOT_TIMER_BEGIN(label) do { } while (0)
#  define AOT_TIMER_END(label, event, component, extraFmt, ...) do { } while (0)
#endif

}  // namespace js::jit

#endif  // jit_AOTInstrumentation_h
