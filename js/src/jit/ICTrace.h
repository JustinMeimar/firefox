/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ICTrace_h
#define jit_ICTrace_h

#include <stdint.h>

class JSScript;

namespace js {
namespace jit {

class BaselineFrame;
class ICCacheIRStub;
class ICFallbackStub;
class ICScript;
class MInstruction;
enum class CacheKind : uint8_t;
enum class CacheOp : uint16_t;

// Binary trace of baseline IC activity, for offline study of fusible regions.
// The format is described in ICTrace.cpp.
bool ICTraceOpen(const char* path);
void ICTraceClose();
bool ICTraceEnabled();

void ICTraceStubEntry(ICCacheIRStub* stub, void* framePtr);
void ICTraceStubFailure(ICCacheIRStub* stub, void* framePtr);
void ICTraceFallback(BaselineFrame* frame, ICFallbackStub* stub);
void ICTraceAttach(ICCacheIRStub* stub, ICScript* icScript,
                   ICFallbackStub* fallback, CacheKind kind);
void ICTraceClone(ICCacheIRStub* newStub, ICCacheIRStub* oldStub);
void ICTraceTranspile(CacheOp op);
void ICTraceMIR(CacheOp op, MInstruction* ins);

}  // namespace jit
}  // namespace js

#endif /* jit_ICTrace_h */
