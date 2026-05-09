/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTInstrumentation_h
#define jit_AOTInstrumentation_h

#include "mozilla/Likely.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace js::jit {

enum AOTInstrCh : uint32_t {
  AOTInstr_IC        = 1 << 0,
  AOTInstr_Lifecycle = 1 << 1,
  AOTInstr_All       = 0xFFFFFFFF,
};

struct AOTInstrumentation {
  uint32_t channels = 0;
  FILE* out = nullptr;

  void init() {
    const char* env = getenv("JS_AOT_INSTR");
    if (!env) return;
    out = stderr;
    if (strcmp(env, "1") == 0 || strcmp(env, "all") == 0) {
      channels = AOTInstr_All;
    } else {
      channels = 0;
      if (strstr(env, "ic")) channels |= AOTInstr_IC;
      if (strstr(env, "lifecycle")) channels |= AOTInstr_Lifecycle;
      if (!channels) channels = AOTInstr_All;
    }
  }

  bool enabled(uint32_t ch) const { return (channels & ch) != 0; }

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
      fprintf(::js::jit::gAOTInstr.out, fmt, ##__VA_ARGS__);    \
    }                                                            \
  } while (0)

}  // namespace js::jit

#endif  // jit_AOTInstrumentation_h
