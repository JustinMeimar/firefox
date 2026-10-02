/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SpecializationTrace_h
#define jit_SpecializationTrace_h

#include <stdint.h>

#include "js/TypeDecls.h"

namespace js::jit {

class ICScript;
class ICCacheIRStub;
class MIRGenerator;
class WarpScriptSnapshot;
class TypeDataList;

void InitSpecializationTrace(JSScript* script, ICScript* icScript);
uint64_t TraceSpecializationProfile(JSScript* script, ICScript* icScript,
                                    uint64_t compilation, uint64_t parent,
                                    uint32_t callOffset, int32_t osrOffset);
void TraceSpecializationProfileEnd(uint64_t compilation, uint64_t context,
                                   WarpScriptSnapshot* snapshot);
void TraceSpecializationDecision(uint64_t compilation, uint64_t context,
                                 uint32_t offset, const char* decision);
void TraceSpecializationTypes(uint64_t compilation, uint64_t context,
                              uint32_t offset, const TypeDataList& types);
void TraceSpecializationSelected(ICScript* icScript, ICCacheIRStub* stub,
                                 uint32_t offset, uint64_t compilation,
                                 uint64_t context);
uint64_t BeginSpecializationCompilation(JSScript* script);
void TraceSpecializationCompilation(MIRGenerator* mir, const char* phase,
                                    double durationUs, bool success);
void TraceSpecializationDependencies(MIRGenerator* mir);
void TraceSpecializationLink(JSScript* script, MIRGenerator* mir,
                             double durationUs, bool success);
void TraceSpecializationBailout(JSScript* script, const char* kind);
void TraceSpecializationInvalidation(JSScript* script);

}  // namespace js::jit

#endif  // jit_SpecializationTrace_h
