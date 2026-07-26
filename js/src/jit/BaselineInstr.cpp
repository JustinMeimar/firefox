/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineInstr.h"

#include <cstddef>
#include <cstdint>

#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/Instr.h"
#include "jit/JitCode.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/Scope.h"

#include "vm/JSScript-inl.h"

namespace js::jit {

// Distinct 4-byte tag on the canonical byte string. Guards against a
// truncated read of the canonical bytes being mistaken for a valid
// header of some other kind.
static constexpr uint32_t kBaselineCanonicalMagic = 0x424C4E63;  // 'BLNc'

Sha1Digest ComputeBaselineSemanticId(JSScript* script) {
  Sha1Mixer m;

  const uint32_t magic = kBaselineCanonicalMagic;
  m.AppendPod(magic);

  const uint32_t immFlags = script->immutableFlags().toRaw();
  m.AppendPod(immFlags);

  const uint32_t mutFlagsMask =
      script->hasDebugScript()
          ? uint32_t(MutableScriptFlagsEnum::HasDebugScript)
          : 0u;
  m.AppendPod(mutFlagsMask);

  const uint32_t funFlags =
      script->function() ? uint32_t(script->function()->flags().toRaw()) : 0u;
  m.AppendPod(funFlags);

  const uint16_t nargs =
      script->function() ? uint16_t(script->function()->nargs()) : uint16_t(0);
  m.AppendPod(nargs);

  const uint16_t nfixed = uint16_t(script->nfixed());
  m.AppendPod(nfixed);

  const uint32_t nslots = uint32_t(script->nslots());
  m.AppendPod(nslots);

  const uint32_t numICEntries = uint32_t(script->numICEntries());
  m.AppendPod(numICEntries);

  auto immData = script->immutableScriptData()->immutableData();
  const uint32_t immDataSize = uint32_t(immData.size());
  m.AppendPod(immDataSize);

  auto gcThings = script->gcthings();
  const uint32_t gcThingCount = uint32_t(gcThings.size());
  m.AppendPod(gcThingCount);

  const uint8_t scopeKind = uint8_t(script->outermostScope()->kind());
  m.AppendPod(scopeKind);

  const uint8_t hasNonSyntactic = script->hasNonSyntacticScope() ? 1 : 0;
  m.AppendPod(hasNonSyntactic);

  const uint8_t isFunction = script->function() ? 1 : 0;
  m.AppendPod(isFunction);

  if (immDataSize) {
    m.Append(immData.data(), immDataSize);
  }
  for (const auto& gct : gcThings) {
    uint8_t k = uint8_t(gct.kind());
    m.AppendPod(k);
  }

  return m.Finish();
}

Sha1Digest ComputeBaselineCodeId(BaselineScript* baseline) {
  Sha1Mixer m;
  JitCode* method = baseline->method();
  if (method) {
    const uint32_t insnBytes = uint32_t(method->instructionsSize());
    m.AppendPod(insnBytes);
    m.Append(method->raw(), insnBytes);
  }
  const uint32_t allocBytes = uint32_t(baseline->allocBytes());
  m.AppendPod(allocBytes);
  return m.Finish();
}

static Sha1Digest StubBodyId(const CacheIRStubInfo* stubInfo) {
  return Sha1(mozilla::Span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(stubInfo->code()),
      stubInfo->codeLength()));
}

// Zero digest reserved for "fallback stub, no CacheIR body". The
// harness knows this is not a real ic_body_id and treats it as the
// distinguished fallback bucket.
static Sha1Digest FallbackBodyId() {
  Sha1Digest d;
  memset(d.bytes, 0, sizeof(d.bytes));
  return d;
}

void HarvestIcChain(ICEntry* icEntry, ICFallbackStub* fallback,
                    JSScript* outerScript, IcDetachReason reason) {
  if (!JSInstr::Enabled(InstrCh_IC)) return;
  if (!icEntry || !fallback || !outerScript) return;

  const uint32_t bcOffset = fallback->pcOffset();
  const uint32_t chainLen = uint32_t(fallback->numOptimizedStubs()) + 1;

  ICStub* s = icEntry->firstStub();
  while (s && !s->isFallback()) {
    ICCacheIRStub* cs = s->toCacheIRStub();
    JSInstr::LogIcInstanceDetach(outerScript, bcOffset,
                                 StubBodyId(cs->stubInfo()), reason,
                                 cs->enteredCount(), /*isFallback=*/false,
                                 chainLen);
    s = cs->next();
  }
  JSInstr::LogIcInstanceDetach(outerScript, bcOffset, FallbackBodyId(), reason,
                               fallback->enteredCount(), /*isFallback=*/true,
                               chainLen);
}

void HarvestOneIcStub(ICCacheIRStub* stub, ICFallbackStub* fallback,
                      JSScript* outerScript, IcDetachReason reason) {
  if (!JSInstr::Enabled(InstrCh_IC)) return;
  if (!stub || !fallback || !outerScript) return;
  const uint32_t bcOffset = fallback->pcOffset();
  const uint32_t chainLen = uint32_t(fallback->numOptimizedStubs()) + 1;
  const Sha1Digest bodyId = StubBodyId(stub->stubInfo());
  JSInstr::LogIcInstanceDetach(outerScript, bcOffset, bodyId, reason,
                               stub->enteredCount(), /*isFallback=*/false,
                               chainLen);
}

// Classify a stub-data field type for the coupling census.
//
// - direct-relocatable: a raw scalar / constant; can be baked into
//   patched code with no runtime lookup.
// - table: pointer to something with a stable per-process identity
//   (JitCode, AllocSite) that can go through an indirection table.
// - instance: pointer to per-instance runtime state (Shape,
//   JSObject, ICScript, Value, etc.); cannot be shared across
//   instances without patching.
static const char* EligibilityOf(StubField::Type ty) {
  switch (ty) {
    case StubField::Type::RawInt32:
    case StubField::Type::RawPointer:
    case StubField::Type::RawInt64:
    case StubField::Type::Double:
    case StubField::Type::Id:
      return "direct-relocatable";
    case StubField::Type::JitCode:
    case StubField::Type::AllocSite:
      return "table";
    case StubField::Type::ICScript:
    case StubField::Type::Shape:
    case StubField::Type::WeakShape:
    case StubField::Type::JSObject:
    case StubField::Type::WeakObject:
    case StubField::Type::Symbol:
    case StubField::Type::String:
    case StubField::Type::WeakBaseScript:
    case StubField::Type::Value:
    case StubField::Type::WeakValue:
      return "instance";
    case StubField::Type::Limit:
      break;
  }
  return "unknown";
}

static const char* RelocKindOf(StubField::Type ty) {
  switch (ty) {
    case StubField::Type::Shape:
    case StubField::Type::WeakShape:
    case StubField::Type::JSObject:
    case StubField::Type::WeakObject:
    case StubField::Type::Symbol:
    case StubField::Type::String:
    case StubField::Type::WeakBaseScript:
    case StubField::Type::JitCode:
    case StubField::Type::Value:
    case StubField::Type::WeakValue:
    case StubField::Type::AllocSite:
      return "gcptr";
    case StubField::Type::ICScript:
    case StubField::Type::RawPointer:
      return "cellptr";
    case StubField::Type::RawInt32:
    case StubField::Type::RawInt64:
    case StubField::Type::Double:
    case StubField::Type::Id:
      return "none";
    case StubField::Type::Limit:
      break;
  }
  return "unknown";
}

void EmitIcBodyIfNew(CacheKind kind, const CacheIRStubInfo* stubInfo) {
  if (!JSInstr::Enabled(InstrCh_IC)) return;
  if (!stubInfo) return;

  Sha1Digest bodyId = StubBodyId(stubInfo);

  Vector<CouplingRecord, 16, SystemAllocPolicy> records;
  uint32_t offset = 0;
  for (size_t i = 0;; ++i) {
    StubField::Type ty = stubInfo->fieldType(i);
    if (ty == StubField::Type::Limit) break;
    CouplingRecord r{
        /*operandKind=*/"StubField",
        /*patchOffset=*/offset,
        /*targetKind=*/StubFieldTypeName(ty),
        /*relocKind=*/RelocKindOf(ty),
        /*eligibility=*/EligibilityOf(ty),
    };
    if (!records.append(r)) {
      break;
    }
    offset += uint32_t(StubField::sizeInBytes(ty));
  }

  JSInstr::LogIcBodyEmit(
      bodyId, CacheKindNames[uint8_t(kind)], stubInfo->codeLength(),
      uint32_t(stubInfo->stubDataSize()),
      mozilla::Span<const CouplingRecord>(records.begin(), records.length()));
}

void EmitBaselineCompileEvent(JSContext* cx, JSScript* script) {
  if (!JSInstr::Enabled(InstrCh_Baseline)) return;
  if (!script->hasBaselineScript()) return;

  BaselineScript* bs = script->baselineScript();

  Sha1Digest semanticId = ComputeBaselineSemanticId(script);
  Sha1Digest codeId = ComputeBaselineCodeId(bs);

  // allocBytes covers the BaselineScript struct plus its trailing
  // arrays (retAddr/osrEntries/debugTrapEntries/etc). The compiled
  // machine code lives in bs->method(), a separate JitCode cell whose
  // size is instructionsSize(). They are disjoint; the harness sums
  // them for total footprint per compile.
  const uint32_t methodBytes =
      bs->method() ? uint32_t(bs->method()->instructionsSize()) : 0u;
  const uint32_t metadataBytes = uint32_t(bs->allocBytes());

  JSInstr::LogBaselineCompile(cx->runtime(), script, semanticId, codeId,
                              methodBytes, metadataBytes,
                              uint32_t(script->numICEntries()));
}

}  // namespace js::jit
