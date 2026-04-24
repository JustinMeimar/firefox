/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTInstrumentation_h
#define jit_AOTInstrumentation_h

#include <cstdio>
#include <cstdlib>

namespace js::jit {

/// NOTE: This file is purely for research purposes. To be
/// deleted eventually, along with the Instrumentation spew
/// channel.
struct AOTInstrumentation {
  bool enabled = false;
  FILE* out = nullptr;

  void init() {
    if (getenv("JS_AOT_INSTR")) {
      enabled = true;
      out = stderr;
    }
  }

  void close() {
    if (out && out != stderr && out != stdout) {
      fclose(out);
    }
    out = nullptr;
    enabled = false;
  }
};

inline AOTInstrumentation gAOTInstr;

#define AOT_INSTR(fmt, ...)                                    \
  do {                                                         \
    if (MOZ_UNLIKELY(::js::jit::gAOTInstr.enabled)) {         \
      fprintf(::js::jit::gAOTInstr.out, fmt, ##__VA_ARGS__);  \
    }                                                          \
  } while (0)

}  // namespace js::jit

#endif  // jit_AOTInstrumentation_h
