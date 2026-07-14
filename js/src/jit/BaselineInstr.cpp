/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineInstr.h"

#include "mozilla/HashFunctions.h"

#include <cstdint>
#include <string>

#include "jit/BaselineJIT.h"
#include "jit/Instr.h"
#include "jit/JitCode.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"
#include "vm/JSAtomUtils.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/Scope.h"

#include "vm/JSScript-inl.h"

using mozilla::HashBytes;
using JS::UniqueChars;

namespace js::jit {

// Magic prefix on the per-blob canonical byte string. Distinct from
// any container header magic so a truncated read of canonical bytes
// cannot be confused with one.
static constexpr uint32_t kBaselineCanonicalMagic = 0x424C4E63;  // 'BLNc'

uint32_t ComputeBaselineProbeHash(JSScript* script) {
  return uint32_t(script->sharedData()->hash());
}

// Canonical bytes identify a script for baseline codegen matching:
// two scripts hash equal iff their baseline codegen output would be
// byte-identical. HasDebugScript is masked in because it toggles trap
// emission; other mutable flags don't affect codegen.
static bool ComputeBaselineCanonical(
    JSScript* script, Vector<uint8_t, 0, SystemAllocPolicy>& out) {
  auto append = [&](const void* p, size_t n) {
    return out.append(reinterpret_cast<const uint8_t*>(p), n);
  };

  auto immData = script->immutableScriptData()->immutableData();
  auto gcThings = script->gcthings();

  uint32_t magic = kBaselineCanonicalMagic;
  uint32_t immFlags = script->immutableFlags().toRaw();
  uint32_t mutFlagsMask =
      script->hasDebugScript()
          ? uint32_t(MutableScriptFlagsEnum::HasDebugScript)
          : 0u;
  uint32_t funFlags =
      script->function() ? uint32_t(script->function()->flags().toRaw()) : 0u;
  uint16_t nargs =
      script->function() ? uint16_t(script->function()->nargs()) : uint16_t(0);
  uint16_t nfixed = uint16_t(script->nfixed());
  uint32_t nslots = uint32_t(script->nslots());
  uint32_t numICEntries = uint32_t(script->numICEntries());
  uint32_t immDataSize = uint32_t(immData.size());
  uint32_t gcThingKindsSize = uint32_t(gcThings.size());
  uint8_t scopeKind = uint8_t(script->outermostScope()->kind());
  uint8_t hasNonSyntactic = script->hasNonSyntacticScope() ? 1 : 0;
  uint8_t isFunction = script->function() ? 1 : 0;
  uint8_t reserved = 0;

  if (!append(&magic, sizeof(magic))) return false;
  if (!append(&immFlags, sizeof(immFlags))) return false;
  if (!append(&mutFlagsMask, sizeof(mutFlagsMask))) return false;
  if (!append(&funFlags, sizeof(funFlags))) return false;
  if (!append(&nargs, sizeof(nargs))) return false;
  if (!append(&nfixed, sizeof(nfixed))) return false;
  if (!append(&nslots, sizeof(nslots))) return false;
  if (!append(&numICEntries, sizeof(numICEntries))) return false;
  if (!append(&immDataSize, sizeof(immDataSize))) return false;
  if (!append(&gcThingKindsSize, sizeof(gcThingKindsSize))) return false;
  if (!append(&scopeKind, sizeof(scopeKind))) return false;
  if (!append(&hasNonSyntactic, sizeof(hasNonSyntactic))) return false;
  if (!append(&isFunction, sizeof(isFunction))) return false;
  if (!append(&reserved, sizeof(reserved))) return false;

  if (immDataSize && !append(immData.data(), immDataSize)) {
    return false;
  }

  for (const auto& gct : gcThings) {
    uint8_t k = uint8_t(gct.kind());
    if (!append(&k, sizeof(k))) return false;
  }

  return true;
}

void EmitBaselineCompileEvent(JSContext* cx, JSScript* script) {
  if (!gJSInstr.enabled(JSInstr_Baseline)) return;
  if (!script->hasBaselineScript()) return;

  BaselineScript* bs = script->baselineScript();

  Vector<uint8_t, 0, SystemAllocPolicy> canonical;
  if (!ComputeBaselineCanonical(script, canonical)) return;
  uint32_t canonicalHash =
      uint32_t(HashBytes(canonical.begin(), canonical.length()));
  uint32_t probeHash = ComputeBaselineProbeHash(script);

  std::string name;
  if (script->function()) {
    if (JSAtom* atom = script->function()->maybePartialDisplayAtom()) {
      UniqueChars printable = AtomToPrintableString(cx, atom);
      if (printable) name = printable.get();
    }
  }
  if (name.empty()) name = "top_level";

  JS_INSTR(JSInstr_Baseline,
           "baseline-compile hash=%u probe=%u code=%u canonical=%zu "
           "nargs=%u scope=%u name=%s\n",
           unsigned(canonicalHash), unsigned(probeHash),
           unsigned(bs->method()->instructionsSize()), canonical.length(),
           script->function() ? unsigned(script->function()->nargs()) : 0u,
           unsigned(script->outermostScope()->kind()), name.c_str());
}

}  // namespace js::jit
