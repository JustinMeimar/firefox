/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCacheTrace_h
#define jit_BaselineCacheTrace_h

#include <stdint.h>

#include "js/TypeDecls.h"

namespace js::jit {

class BaselineScript;

void TraceBaselineCompile(JSScript* script, const char* phase,
                          int64_t startTime, bool success);
void TraceBaselineInstall(JSScript* script, BaselineScript* baseline,
                          int64_t startTime, int64_t installTime);
void TraceBaselineDiscard(JSScript* script, BaselineScript* baseline);

}  // namespace js::jit

#endif  // jit_BaselineCacheTrace_h
