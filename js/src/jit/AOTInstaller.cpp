/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTInstaller.h"

#  include "mozilla/ScopeExit.h"

#  include "jit/AOT.h"
#  include "jit/AOTCompilationKey.h"
#  include "jit/AOTImage.h"
#  include "jit/AOTImageGenerated.h"
#  include "jit/AutoWritableJitCode.h"
#  include "jit/BaselineCodeGen.h"
#  include "jit/BaselineIC.h"
#  include "jit/BaselineJIT.h"
#  include "jit/CacheIRCompiler.h"
#  include "jit/Ion.h"
#  include "jit/IonOptimizationLevels.h"
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
#  include "vm/SharedStencil.h"

#  include "jit/JitScript-inl.h"
#  include "vm/JSScript-inl.h"

namespace js::jit {

uint32_t ComputeBaselineProbeHash(JSScript* script) {
  return uint32_t(script->sharedData()->hash());
}

template <typename Matches>
static mozilla::Maybe<AOTBlobReader> FindAOTArtifact(AOTBlobKind kind,
                                                     Matches matches) {
  const AOTImage* image = AOTImage::embedded();
  if (image) {
    for (uint32_t i = 0; i < image->blobCount(); i++) {
      auto candidate = image->blobAt(i);
      if (candidate.kind() == kind && matches(candidate)) {
        return mozilla::Some(candidate);
      }
    }
  }
  return mozilla::Nothing();
}

bool InstallAOTBaselineInterpreter(JSContext* cx, BaselineInterpreter& interp) {
  MOZ_ASSERT(cx->runtime()->jitRuntime()->aotPolicy().shouldLoad(
      AOTBlobKind::BaselineInterpreter));

  auto reader = FindAOTArtifact(
      AOTBlobKind::BaselineInterpreter, [&](const AOTBlobReader& candidate) {
        AOTCompilationKey key(candidate.key());
        WriteAOTContext(key, candidate.kind(), JitOptions,
                        cx->runtime()->geckoProfiler().enabled());
        return key.complete();
      });
  if (!reader) {
    return false;
  }
  BaselineInterpreterMetadata md;
  if (!DecodeBlob_BaselineInterpreter(*reader, &md)) {
    return false;
  }

  auto code = reader->code();
  uint8_t* codeStart = const_cast<uint8_t*>(code.data());
  JitCode* jitCode =
      JitCode::NewStatic(cx, codeStart, uint32_t(code.size()), CodeKind::Other);
  if (!jitCode) {
    return false;
  }

  JitRuntime* jrt = cx->runtime()->jitRuntime();
  JitCode* trampoline =
      jrt->generateAOTPreambleTrampoline(cx, jitCode->raw(), AOTInterpPassReg);
  if (!trampoline) {
    return false;
  }
  // Register the static interpreter code with the profiler and enable its
  // instrumentation.
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

  jrt->aotInterpPreambleTrampoline_ = trampoline;
  interp.init(jitCode, std::move(md));

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

// Finds the matching baseline function artifact and installs its static code.
// On failure the script remains unchanged.
bool TryInstallAOTBaselineScript(JSContext* cx, JS::HandleScript script) {
  if (!cx->runtime()->jitRuntime()->aotPolicy().shouldLoad(
          AOTBlobKind::BaselineFunction)) {
    return false;
  }

  uint32_t probe = ComputeBaselineProbeHash(script);
  uint32_t warmUpThreshold =
      OptimizationInfo::baseWarmUpThresholdForScript(cx, script);
  bool ionCompileable = IsIonEnabled(cx) && CanIonCompileScript(cx, script);
  auto reader = FindAOTArtifact(
      AOTBlobKind::BaselineFunction, [&](const AOTBlobReader& candidate) {
        if (candidate.entry()->probeHash != probe) {
          return false;
        }
        AOTCompilationKey key(candidate.key());
        WriteAOTContext(key, candidate.kind(), JitOptions,
                        cx->runtime()->geckoProfiler().enabled());
        WriteAOTBaselineInputs(key, script, warmUpThreshold, ionCompileable,
                               false);
        return key.complete();
      });
  if (!reader) {
    return false;
  }

  // The script may not have its baseline metadata initialized when AOT
  // installation begins. Initialize it before installing the compiled code.
  if (!cx->zone()->ensureJitZoneExists(cx)) {
    return false;
  }
  AutoKeepJitScripts keepJitScript(cx);
  if (!script->ensureHasJitScript(cx, keepJitScript)) {
    return false;
  }
  if (!script->jitScript()->ensureHasCachedBaselineJitData(cx, script)) {
    return false;
  }

  BaselineScriptMetadata md;
  if (!DecodeBlob_BaselineFunction(*reader, &md)) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT baseline function decode failed for %s:%u",
            script->filename() ? script->filename() : "<null>",
            unsigned(script->lineno()));
    return false;
  }

  auto code = reader->code();
  uint8_t* codeStart = const_cast<uint8_t*>(code.data());
  JitCode* jitCode = JitCode::NewStatic(cx, codeStart, uint32_t(code.size()),
                                        CodeKind::Baseline);
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

  auto destroy = mozilla::MakeScopeExit(
      [&] { BaselineScript::Destroy(cx->gcContext(), bs); });
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

  // Register the static baseline code with the profiler so stack walkers can
  // associate return addresses with the script.
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

  if (md.disableIon) {
    script->disableIon();
  }
  if (md.uninlineable) {
    script->setUninlineable();
  }
  script->jitScript()->setRanBytecodeAnalysis();
  script->jitScript()->setIonThreshold(warmUpThreshold);
  script->jitScript()->setBaselineScript(script, bs);
  destroy.release();
  FinalizeInstalledBaselineScript(script);

  JitSpew(JitSpew_BaselineAOT,
          "installed baseline function from AOT image: %s:%u bytes=%zu",
          script->filename() ? script->filename() : "<null>",
          unsigned(script->lineno()), size_t(code.size()));
  return true;
}

// IC stubs

bool TryLoadAOTICStubs(JSContext* cx, JitZone* jitZone) {
  if (!cx->runtime()->jitRuntime()->aotPolicy().shouldLoad(
          AOTBlobKind::InlineCacheStub)) {
    return false;
  }

  const AOTImage* image = AOTImage::embedded();
  if (!image) {
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

    AOTCompilationKey keyInputs(reader.key());
    WriteAOTContext(keyInputs, reader.kind(), JitOptions, false);
    WriteAOTICInputs(keyInputs, md);
    if (!keyInputs.complete()) {
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

    CacheIRStubKey key(stubInfo);
    CacheIRStubKey::Lookup lookup(CacheKind(md.cacheKind),
                                  ICStubEngine::Baseline, stubInfo->code(),
                                  stubInfo->codeLength());

    CacheIRStubInfo* existing = nullptr;
    if (jitZone->getBaselineCacheIRStubCode(lookup, &existing)) {
      // Ignore an AOT stub when equivalent runtime code already exists.
      continue;
    }

    auto codeSpan = reader.code();
    JitCode* jitCode =
        JitCode::NewStatic(cx, const_cast<uint8_t*>(codeSpan.data()),
                           uint32_t(codeSpan.size()), CodeKind::Baseline);
    if (!jitCode) {
      return false;
    }
    jitCode->setLocalTracingSlots(md.localTracingSlots);

    if (!jitZone->putBaselineCacheIRStubCode(lookup, key, jitCode)) {
      // The cache takes ownership only after a successful insertion. The
      // temporary key retains ownership on failure.
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
