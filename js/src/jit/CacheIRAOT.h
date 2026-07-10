/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRAOT_h
#define jit_CacheIRAOT_h

#include "mozilla/Span.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRWriter.h"

struct JSContext;

namespace js::jit {

class JitZone;

struct AOTStubFieldData {
  StubField::Type type;
  uint64_t data;
};

struct CacheIRAOTStub {
  CacheKind kind;
  uint32_t numOperandIds;
  uint32_t numInputOperands;
  uint32_t numInstructions;
  TypeData typeData;
  uint32_t stubDataSize;
  const AOTStubFieldData* stubfields;
  size_t stubfieldCount;
  const uint32_t* operandLastUsed;
  const uint8_t* data;
  size_t dataLength;
};

mozilla::Span<const CacheIRAOTStub> GetAOTStubs();
// NOTE(aot): Must be called while in the atoms zone. AOT ICs live there.
void FillAOTICs(JSContext* cx);

struct CacheIRAOTHint {
  uint32_t scriptKey;
  uint32_t pcOffset;
  uint32_t corpusIdx;
};

mozilla::Span<const CacheIRAOTHint> GetAOTEagerICHintsForScript(
    uint32_t scriptKey);

}  // namespace js::jit

#endif /* jit_CacheIRAOT_h */
