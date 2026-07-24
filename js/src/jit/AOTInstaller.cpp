/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTInstaller.h"

#  include "mozilla/SHA1.h"

#  include <cstring>

#  include "jit/AOT.h"
#  include "jit/AOTImage.h"
#  include "jit/AOTImageGenerated.h"
#  include "jit/AutoWritableJitCode.h"
#  include "jit/BaselineCodeGen.h"
#  include "jit/BaselineIC.h"
#  include "jit/BaselineJIT.h"
#  include "jit/CacheIRCompiler.h"
#  include "jit/JitCode.h"
#  include "jit/JitcodeMap.h"
#  include "jit/JitOptions.h"
#  include "jit/JitRuntime.h"
#  include "jit/JitScript.h"
#  include "jit/JitSpewer.h"
#  include "jit/JitZone.h"
#  include "vm/CodeCoverage.h"
#  include "vm/GeckoProfiler.h"
#  include "vm/JSContext.h"
#  include "vm/JSScript.h"
#  include "vm/Scope.h"
#  include "vm/SharedStencil.h"

#  include "jit/JitScript-inl.h"
#  include "vm/JSScript-inl.h"

namespace js::jit {

static bool IsAOTImageCompatible(const AOTImage* image) {
  auto readerOpt = image->findUnique(AOTBlobKind::Configuration);
  if (readerOpt.isNothing()) {
    JitSpew(JitSpew_BaselineAOT, "AOT image lacks configuration metadata");
    return false;
  }

  AOTBlobReader reader = readerOpt.ref();
  AOTConfigurationMetadata recorded;
  if (!DecodeBlob_Configuration(reader, &recorded)) {
    JitSpew(JitSpew_BaselineAOT, "AOT image configuration decode failed");
    return false;
  }
  if (recorded != CurrentAOTConfiguration()) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT image configuration mismatch; using runtime codegen");
    return false;
  }
  return true;
}

// -----------------------------------------------------------------
// Identity
// -----------------------------------------------------------------

uint32_t ComputeBaselineProbeHash(JSScript* script) {
  return uint32_t(script->sharedData()->hash());
}

void ComputeBaselineIdentityHash(JSScript* script,
                                 mozilla::SHA1Sum::Hash& out) {
  mozilla::SHA1Sum sha;
  auto u = [&](const void* p, size_t n) { sha.update(p, uint32_t(n)); };

  uint32_t immFlags = script->immutableFlags().toRaw();
  uint32_t funFlags =
      script->function() ? uint32_t(script->function()->flags().toRaw()) : 0u;
  uint16_t nargs =
      script->function() ? uint16_t(script->function()->nargs()) : uint16_t(0);
  uint16_t nfixed = uint16_t(script->nfixed());
  uint32_t nslots = uint32_t(script->nslots());
  uint32_t numICEntries = uint32_t(script->numICEntries());
  uint8_t scopeKind = uint8_t(script->outermostScope()->kind());
  uint8_t hasNonSyntactic = script->hasNonSyntacticScope() ? 1 : 0;
  uint8_t isFunction = script->function() ? 1 : 0;

  u(&immFlags, sizeof(immFlags));
  u(&funFlags, sizeof(funFlags));
  u(&nargs, sizeof(nargs));
  u(&nfixed, sizeof(nfixed));
  u(&nslots, sizeof(nslots));
  u(&numICEntries, sizeof(numICEntries));
  u(&scopeKind, sizeof(scopeKind));
  u(&hasNonSyntactic, sizeof(hasNonSyntactic));
  u(&isFunction, sizeof(isFunction));

  // Length-prefix the gcthing kinds so the tail of variable-length
  // bytecode does not alias.
  auto gcThings = script->gcthings();
  uint32_t gcThingCount = uint32_t(gcThings.size());
  u(&gcThingCount, sizeof(gcThingCount));
  for (const auto& gct : gcThings) {
    uint8_t k = uint8_t(gct.kind());
    u(&k, 1);
  }

  auto immData = script->immutableScriptData()->immutableData();
  if (!immData.empty()) {
    u(immData.data(), immData.size());
  }

  sha.finish(out);
}

// -----------------------------------------------------------------
// Baseline interpreter
// -----------------------------------------------------------------

bool TryInstallAOTBaselineInterpreter(JSContext* cx,
                                      BaselineInterpreter& interp) {
  if (!JitOptions.useAOTImage) {
    return false;
  }

  const AOTImage* image = AOTImage::embedded();
  if (!image || !IsAOTImageCompatible(image)) {
    return false;
  }

  auto readerOpt = image->findUnique(AOTBlobKind::BaselineInterpreter);
  if (readerOpt.isNothing()) {
    JitSpew(JitSpew_BaselineAOT, "AOT image lacks a baseline interpreter blob");
    return false;
  }

  AOTBlobReader reader = readerOpt.ref();
  BaselineInterpreterMetadata md;
  if (!DecodeBlob_BaselineInterpreter(reader, &md)) {
    JitSpew(JitSpew_BaselineAOT, "AOT baseline interpreter decode failed");
    return false;
  }

  auto code = reader.code();
  uint8_t* codeStart = const_cast<uint8_t*>(code.data());
  JitCode* jitCode =
      AllocateAOTCode(cx, codeStart, uint32_t(code.size()), CodeKind::Other);
  if (!jitCode) {
    return false;
  }

  if (!EnsureAOTPreambleTrampolineFor(cx, jitCode, AOTInterpPassReg)) {
    return false;
  }

  interp.init(jitCode, std::move(md));

  // Register the AOT interpreter .text range with the profiler and
  // enable instrumentation channels that are otherwise wired up during
  // GenerateBaselineInterpreter after normal codegen.
  {
    auto profEntry = MakeJitcodeGlobalEntry<BaselineInterpreterEntry>(
        cx, jitCode, jitCode->raw(), jitCode->rawEnd());
    if (!profEntry) {
      return false;
    }
    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(std::move(profEntry))) {
      ReportOutOfMemory(cx);
      return false;
    }
    jitCode->setHasBytecodeMap();
  }

  if (cx->runtime()->geckoProfiler().enabled()) {
    interp.toggleProfilerInstrumentation(true);
  }
  if (coverage::IsLCovEnabled()) {
    interp.toggleCodeCoverageInstrumentationUnchecked(true);
  }

  JitSpew(JitSpew_BaselineAOT,
          "installed baseline interpreter from AOT image: bytes=%zu",
          size_t(code.size()));
  return true;
}

// -----------------------------------------------------------------
// Baseline JIT function
// -----------------------------------------------------------------

// Finds the matching BaselineFunction blob for `script` and installs a
// BaselineScript over its static text range. Success attaches the
// BaselineScript to the JSScript; failure leaves the script untouched.
bool TryInstallAOTBaselineScript(JSContext* cx, JS::HandleScript script) {
  if (!JitOptions.useAOTImage) {
    return false;
  }

  const AOTImage* image = AOTImage::embedded();
  if (!image || !IsAOTImageCompatible(image)) {
    return false;
  }

  uint32_t probe = ComputeBaselineProbeHash(script);
  mozilla::SHA1Sum::Hash liveHash;
  ComputeBaselineIdentityHash(script, liveHash);

  auto readerOpt =
      image->findByIdentity(AOTBlobKind::BaselineFunction, probe, liveHash);
  if (readerOpt.isNothing()) {
    return false;
  }

  // CanEnterBaselineJIT reaches us before the normal BaselineCompile /
  // PrepareToCompile path runs, so the JitScript and its cached baseline
  // data may not exist yet. Set them up here before touching jitScript().
  AutoKeepJitScripts keepJitScript(cx);
  if (!script->ensureHasJitScript(cx, keepJitScript)) {
    return false;
  }
  if (!script->jitScript()->ensureHasCachedBaselineJitData(cx, script)) {
    return false;
  }

  AOTBlobReader reader = readerOpt.ref();
  BaselineScriptMetadata md;
  if (!DecodeBlob_BaselineFunction(reader, &md)) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT baseline function decode failed for %s:%u",
            script->filename() ? script->filename() : "<null>",
            unsigned(script->lineno()));
    return false;
  }

  auto code = reader.code();
  uint8_t* codeStart = const_cast<uint8_t*>(code.data());
  JitCode* jitCode =
      AllocateAOTCode(cx, codeStart, uint32_t(code.size()), CodeKind::Baseline);
  if (!jitCode) {
    return false;
  }

  if (!EnsureAOTPreambleTrampolineFor(cx, jitCode, AOTFuncPassReg)) {
    return false;
  }
  uint8_t* trampoline =
      cx->runtime()->jitRuntime()->lookupAOTPreambleTrampoline(jitCode->raw());
  MOZ_ASSERT(trampoline);

  BaselineScript* bs = BaselineScript::New(
      cx, md.warmUpCheckPrologueOffset, md.profilerEnterToggleOffset,
      md.profilerExitToggleOffset, md.retAddrEntries.length(),
      md.osrEntries.length(), md.debugTrapEntries.length(),
      script->resumeOffsets().size());
  if (!bs) {
    return false;
  }

  bs->setMethod(jitCode);
  bs->setAOTPreambleTrampoline(trampoline);
  if (!md.retAddrEntries.empty()) {
    bs->copyRetAddrEntries(md.retAddrEntries.begin());
  }
  if (!md.osrEntries.empty()) {
    bs->copyOSREntries(md.osrEntries.begin());
  }
  if (!md.debugTrapEntries.empty()) {
    bs->copyDebugTrapEntries(md.debugTrapEntries.begin());
  }
  bs->computeResumeNativeOffsets(script, md.resumeOffsetEntries);

  if (md.flags & BaselineScript::HAS_DEBUG_INSTRUMENTATION) {
    bs->setHasDebugInstrumentation();
  }

  script->jitScript()->setBaselineScript(script, bs);

  FinalizeInstalledBaselineScript(script);

  // Register the AOT baseline .text range with the profiler so stack
  // walks can map return addresses back to a script. Without this,
  // profiler frame classification fails for anything running in
  // AOT-installed baseline code.
  JitcodeGlobalTable* globalTable =
      cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
  if (!globalTable->lookup(jitCode->raw())) {
    UniqueChars str = GeckoProfilerRuntime::allocProfileString(cx, script);
    if (!str) {
      return false;
    }
    auto profEntry = MakeJitcodeGlobalEntry<RealmIndependentSharedEntry>(
        cx, jitCode, jitCode->raw(), jitCode->rawEnd(), std::move(str));
    if (!profEntry) {
      return false;
    }
    if (!globalTable->addEntry(std::move(profEntry))) {
      ReportOutOfMemory(cx);
      return false;
    }
    jitCode->setHasBytecodeMap();
  }

  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    AutoWritableJitCode awjc(bs->method());
    bs->toggleProfilerInstrumentation(true);
  }

  JitSpew(JitSpew_BaselineAOT,
          "installed baseline function from AOT image: %s:%u bytes=%zu",
          script->filename() ? script->filename() : "<null>",
          unsigned(script->lineno()), size_t(code.size()));
  return true;
}

// -----------------------------------------------------------------
// IC stubs
// -----------------------------------------------------------------

bool TryLoadAOTICStubs(JSContext* cx, JitZone* jitZone) {
  if (!JitOptions.useAOTImage) {
    return false;
  }

  const AOTImage* image = AOTImage::embedded();
  if (!image || !IsAOTImageCompatible(image)) {
    return false;
  }

  uint32_t loaded = 0;
  uint32_t attempted = 0;
  for (uint32_t i = 0; i < image->blobCount(); i++) {
    AOTBlobReader reader = image->blobAt(i);
    if (reader.kind() != AOTBlobKind::InlineCacheStub) {
      continue;
    }
    attempted++;

    AOTICStubMetadata md;
    if (!DecodeBlob_InlineCacheStub(reader, &md)) {
      if (cx->isExceptionPending()) {
        return false;
      }
      JitSpew(JitSpew_BaselineAOT, "AOT IC stub decode failed at index %u", i);
      continue;
    }

    CacheIRStubInfo* stubInfo = CacheIRStubInfo::NewFromSerialized(
        CacheKind(md.cacheKind), ICStubEngine::Baseline, md.makesGCCalls != 0,
        md.stubDataOffset, md.cacheIRCode.begin(), md.cacheIRCode.length(),
        md.fieldTypes.begin(), md.fieldTypes.length());
    if (!stubInfo) {
      if (cx->isExceptionPending()) {
        return false;
      }
      continue;
    }

    CacheIRStubKey::Lookup lookup(CacheKind(md.cacheKind),
                                  ICStubEngine::Baseline, stubInfo->code(),
                                  stubInfo->codeLength());

    CacheIRStubInfo* existing = nullptr;
    if (jitZone->getBaselineCacheIRStubCode(lookup, &existing)) {
      // Runtime already has this stub; the AOT copy is redundant.
      js_free(stubInfo);
      continue;
    }

    auto codeSpan = reader.code();
    JitCode* jitCode =
        AllocateAOTCode(cx, const_cast<uint8_t*>(codeSpan.data()),
                        uint32_t(codeSpan.size()), CodeKind::Baseline);
    if (!jitCode) {
      js_free(stubInfo);
      return false;
    }
    jitCode->setLocalTracingSlots(md.localTracingSlots);

    CacheIRStubKey key(stubInfo);
    if (!jitZone->putBaselineCacheIRStubCode(lookup, key, jitCode)) {
      // putBaselineCacheIRStubCode takes ownership on success only;
      // on failure the key's UniquePtr will drop stubInfo.
      if (cx->isExceptionPending()) {
        return false;
      }
      continue;
    }
    loaded++;
  }

  if (attempted > 0) {
    JitSpew(JitSpew_BaselineAOT, "AOT IC stubs loaded=%u attempted=%u", loaded,
            attempted);
  }
  return loaded > 0;
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT
