/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"

#include "mozilla/SHA1.h"

#include <cstdint>
#include <cstring>
#include <fstream>

#include "frontend/CompilationStencil.h"
#include "gc/Zone.h"
#include "jit/AOT.h"
#include "jit/AOTInstrumentation.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/BaselineCodeGen.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRCompiler.h"
#include "jit/JitCode.h"
#include "jit/JitcodeMap.h"
#include "jit/JitContext.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/VMFunctions.h"
#include "vm/JSAtomUtils.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/Runtime.h"
#include "vm/Scope.h"
#include "vm/SharedStencil.h"

#include "jit/JitScript-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"

namespace js::jit {

#ifdef ENABLE_JS_AOT

// ============================================================
// Script Identity
// ============================================================

// Fast probe key. Collides for scripts sharing bytecode; disambiguated
// by the identity hash below on the load path.
uint32_t ComputeBaselineProbeHash(JSScript* script) {
  return uint32_t(script->sharedData()->hash());
}

// Two scripts hash equal iff a baseline blob compiled for one is
// byte-compatible with the other. Any change to what is hashed, its
// order, or its widths invalidates existing corpora, so bump the
// container version alongside.
static void HashBaselineIdentity(JSScript* script,
                                 mozilla::SHA1Sum::Hash& out) {
  mozilla::SHA1Sum sha;
  auto u = [&](const void* p, size_t n) {
    sha.update(p, uint32_t(n));
  };

  uint32_t immFlags = script->immutableFlags().toRaw();
  uint32_t funFlags = script->function()
                          ? uint32_t(script->function()->flags().toRaw())
                          : 0u;
  uint16_t nargs = script->function()
                       ? uint16_t(script->function()->nargs())
                       : uint16_t(0);
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

  // Length-prefixed so the boundary with the trailing variable-length
  // stream is unambiguous.
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

// ============================================================
// Blob Encode / Decode
// ============================================================


// NOTE(Refactor): AFAICT this is used only when recording a baseline function.
// What is the off process / on file represetnation o f a baseline function we
// will use for this AOT system? Will it be .js, will it be textual bytecode?
// It most definitely can not be the .bin we have now,
[[nodiscard]] static bool EncodeBaselineFunctionBlob(
    HandleScript script, const mozilla::SHA1Sum::Hash& identityHash,
    BaselineScript* bs, AOTBlobWriter& blob) {

  Vector<uint32_t, 0, SystemAllocPolicy> resumeBuf;
  if (!bs->aotResumeOffsets(resumeBuf)) return false;

  JitCode* jitCode = bs->method();
  AOTPayload_BaselineFunction payload{
      .fields = {
          .warmUpCheckPrologueOffset = bs->warmUpCheckPrologueOffset(),
          .profilerEnterToggleOffset = bs->profilerEnterToggleOffset(),
          .profilerExitToggleOffset = bs->profilerExitToggleOffset(),
          .retAddrEntryCount = uint32_t(bs->aotRetAddrEntries().size()),
          .osrEntryCount = uint32_t(bs->aotOSREntries().size()),
          .debugTrapEntryCount = uint32_t(bs->aotDebugTrapEntries().size()),
          .resumeEntryCount = uint32_t(resumeBuf.length()),
          .codeSize = uint32_t(jitCode->instructionsSize()),
          .headerSize = uint32_t(jitCode->headerSize()),
          .nargs = script->function()
                       ? uint16_t(script->function()->nargs())
                       : uint16_t(0),
          .nfixed = uint16_t(script->nfixed()),
          .scopeKind = uint8_t(script->outermostScope()->kind()),
      },
      .code = mozilla::Span(jitCode->raw(), jitCode->instructionsSize()),
      .retAddrs = bs->aotRetAddrEntries(),
      .osrEntries = bs->aotOSREntries(),
      .debugTraps = bs->aotDebugTrapEntries(),
      .resumeOffsets = mozilla::Span<const uint32_t>(resumeBuf.begin(),
                                                     resumeBuf.length()),
  };
  memcpy(payload.fields.identityHash, identityHash,
         mozilla::SHA1Sum::kHashSize);

  return EncodeAOTBlob_BaselineFunction(blob, payload);
}

[[nodiscard]] static BaselineScript* NewAOTBaselineScript(
    JSContext* cx, JitCode* code,
    const AOTPayload_BaselineFunction& payload) {

  const auto& f = payload.fields;
  BaselineScript* bs = BaselineScript::New(
      cx, f.warmUpCheckPrologueOffset, f.profilerEnterToggleOffset,
      f.profilerExitToggleOffset, f.retAddrEntryCount, f.osrEntryCount,
      f.debugTrapEntryCount, f.resumeEntryCount);
  if (!bs) return nullptr;
  bs->setMethod(code);
  if (!payload.retAddrs.empty()) bs->copyRetAddrEntries(payload.retAddrs.data());
  if (!payload.osrEntries.empty()) bs->copyOSREntries(payload.osrEntries.data());
  if (!payload.debugTraps.empty()) {
    bs->copyDebugTrapEntries(payload.debugTraps.data());
  }
  if (!payload.resumeOffsets.empty()) {
    bs->copyResumeEntries(payload.resumeOffsets.data());
  }
  return bs;
}

// ============================================================
// Install Path
// ============================================================

bool EnsureAOTPreambleTrampolineFor(JSContext* cx, JitCode* code) {
  JitRuntime* jrt = cx->runtime()->jitRuntime();
  if (jrt->lookupAOTPreambleTrampoline(code->raw())) return true;

  mozilla::Maybe<JitContext> jctx;
  if (!MaybeGetJitContext()) {
    jctx.emplace(cx);
  }
  JitCode* trampoline =
      jrt->generateAOTPreambleTrampoline(cx, code->raw(), AOTSelfHostedPassReg);
  if (!trampoline) return false;
  if (!jrt->aotPreambleTrampolines_.append(
          JitRuntime::AOTPreambleTrampolineEntry{code->raw(), trampoline})) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

[[nodiscard]] static bool RegisterAOTBaselineForProfiler(JSContext* cx,
                                                        HandleScript script,
                                                        JitCode* code) {
  JitcodeGlobalTable* globalTable =
      cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
  if (globalTable->lookup(code->raw())) return true;

  UniqueChars str = GeckoProfilerRuntime::allocProfileString(cx, script);
  if (!str) return false;
  auto profEntry = MakeJitcodeGlobalEntry<RealmIndependentSharedEntry>(
      cx, code, code->raw(), code->rawEnd(), std::move(str));
  if (!profEntry) return false;
  if (!globalTable->addEntry(std::move(profEntry))) {
    ReportOutOfMemory(cx);
    return false;
  }
  code->setHasBytecodeMap();
  return true;
}

static void MaybeToggleProfilerForAOTBaseline(JSContext* cx,
                                              BaselineScript* bs) {
  if (!cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    return;
  }
  AutoWritableJitCode awjc(bs->method());
  bs->toggleProfilerInstrumentation(true);
}

// Install a decoded BaselineFunction payload on the given JSScript.
// Called by both self-hosted delazify and guest baseline compile.
[[nodiscard]] static bool InstallBaselineScriptPayload(
    JSContext* cx, HandleScript script,
    const AOTBlobDirectoryEntry* entry,
    const AOTPayload_BaselineFunction& payload) {
  JitCode* code =
      AllocateAOTCode(cx, entry, GetAOTTextBase(), CodeKind::Baseline);
  if (!code) return false;

  BaselineScript* bs = NewAOTBaselineScript(cx, code, payload);
  if (!bs) return false;

  if (!EnsureAOTPreambleTrampolineFor(cx, code)) return false;
  bs->setAOTPreambleTrampoline(
      cx->runtime()->jitRuntime()->lookupAOTPreambleTrampoline(code->raw()));

  script->jitScript()->setBaselineScript(script, bs);

  FinalizeInstalledBaselineScript(script);

  if (!RegisterAOTBaselineForProfiler(cx, script, code)) return false;

  MaybeToggleProfilerForAOTBaseline(cx, bs);
  return true;
}

// ============================================================
// Dump Orchestration
// ============================================================

bool BuildAndSaveInterpBlob(JSContext* cx,
                            const AOTPayload_BaselineInterpreter& payload) {
  auto& saved = cx->runtime()->jitRuntime()->aotDump_.interpreterBlob;
  saved.reset();
  saved.emplace(AOTBlobKind::BaselineInterpreter,
                /* nameHash = */ 0, "BaselineInterpreter");

  if (!EncodeAOTBlob_BaselineInterpreter(*saved, payload)) {
    return false;
  }

  JitSpew(JitSpew_BaselineAOT,
          "Saved interpreter blob: code=%zu fields=%zu",
          payload.code.size(),
          sizeof(AOTFields_BaselineInterpreter));
  return true;
}

static bool DriveSelfHostedBaselineForAOT(JSContext* cx,
                                          Handle<JSAtom*> atom,
                                          MutableHandleScript scriptOut) {
  Rooted<PropertyName*> name(cx, atom->asPropertyName());
  auto indexRange = cx->runtime()->getSelfHostedScriptIndexRange(name);
  if (!indexRange) {
    return false;
  }
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

  UniqueChars nameStr = AtomToPrintableString(cx, atom);
  if (!nameStr) {
    return false;
  }

  RootedFunction fun(
      cx, cx->runtime()->selfHostStencil().instantiateSelfHostedLazyFunction(
              cx, cx->runtime()->selfHostStencilInput().atomCache,
              indexRange->start, name));
  if (!fun) {
    return false;
  }
  if (!cx->runtime()->delazifySelfHostedFunction(cx, name, fun)) {
    return false;
  }

  Rooted<JSScript*> script(cx, fun->nonLazyScript());
  MOZ_ASSERT(script);

  if (!CanBaselineInterpretScript(script)) {
    return false;
  }

  if (!cx->zone()->ensureJitZoneExists(cx)) {
    return false;
  }
  AutoKeepJitScripts keepJitScript(cx);
  if (!script->ensureHasJitScript(cx, keepJitScript)) {
    return false;
  }

  BaselineOptions options({BaselineOption::ForceMainThreadCompilation});
  MethodStatus result = BaselineCompile(cx, script, options,
                                        /*isAOTDump=*/true);
  if (result != Method_Compiled) {
    return false;
  }
  MOZ_ASSERT(script->hasBaselineScript());

  scriptOut.set(script);
  return true;
}

void EmitBaselineCompileEvent(JSContext* cx, JSScript* script) {
  if (!gAOTInstr.enabled(AOTInstr_Baseline)) return;
  if (!script->hasBaselineScript()) return;

  BaselineScript* bs = script->baselineScript();

  mozilla::SHA1Sum::Hash identityHash;
  HashBaselineIdentity(script, identityHash);
  uint32_t hashPrefix;
  memcpy(&hashPrefix, identityHash, sizeof(hashPrefix));
  uint32_t probeHash = ComputeBaselineProbeHash(script);

  std::string name;
  if (script->function()) {
    if (JSAtom* atom = script->function()->maybePartialDisplayAtom()) {
      UniqueChars printable = AtomToPrintableString(cx, atom);
      if (printable) name = printable.get();
    }
  }
  if (name.empty()) name = "top_level";

  AOT_INSTR(AOTInstr_Baseline,
            "baseline-compile sha1=%08x probe=%u code=%u "
            "nargs=%u scope=%u name=%s\n",
            hashPrefix, unsigned(probeHash),
            unsigned(bs->method()->instructionsSize()),
            script->function() ? unsigned(script->function()->nargs()) : 0u,
            unsigned(script->outermostScope()->kind()),
            name.c_str());
}

bool RecordAOTBaselineFunction(JSContext* cx, HandleScript script) {
  MOZ_ASSERT(JitOptions.dumpAOTSelfHosted ||
             JitOptions.recordAOTBaselineCorpus);
  MOZ_ASSERT(script->hasBaselineScript());

  BaselineScript* bs = script->baselineScript();

  mozilla::SHA1Sum::Hash identityHash;
  HashBaselineIdentity(script, identityHash);
  AOTHashKey key;
  memcpy(key.bytes, identityHash, sizeof(identityHash));

  auto& accum = cx->runtime()->jitRuntime()->aotDump_;
  {
    LockGuard<Mutex> lock(accum.mutex);
    auto ptr = accum.recordedBaselineIdentities.lookupForAdd(key);
    if (ptr) {
      return true;
    }
    if (!accum.recordedBaselineIdentities.add(ptr, key)) return false;
  }

  std::string name;
  if (script->function()) {
    if (JSAtom* atom = script->function()->maybePartialDisplayAtom()) {
      UniqueChars printable = AtomToPrintableString(cx, atom);
      if (printable) {
        name = printable.get();
      }
    }
  }
  if (name.empty()) {
    name = "top_level";
  }

  uint32_t probeHash = ComputeBaselineProbeHash(script);
  AOTBlobWriter blob(AOTBlobKind::BaselineFunction, probeHash, std::move(name));

  if (!EncodeBaselineFunctionBlob(script, identityHash, bs, blob)) {
    return false;
  }

  uint32_t hashPrefix;
  memcpy(&hashPrefix, identityHash, sizeof(hashPrefix));

  JitSpew(JitSpew_BaselineAOT,
          "AOT baseline function probe=%u sha1=%08x size=%zu "
          "nargs=%u scope=%u '%s'",
          probeHash, hashPrefix, bs->method()->instructionsSize(),
          script->function() ? unsigned(script->function()->nargs()) : 0u,
          unsigned(script->outermostScope()->kind()),
          blob.name().c_str());

  AOT_INSTR(AOTInstr_Baseline,
            "baseline-record sha1=%08x size=%u name=%s\n",
            hashPrefix, unsigned(bs->method()->instructionsSize()),
            blob.name().c_str());

  if (!accum.baselineFunctionBlobs.append(std::move(blob))) return false;
  return true;
}

bool RecordAOTICStub(JSContext* cx, JitCode* code,
                     CacheIRStubInfo* stubInfo) {
  MOZ_ASSERT(JitOptions.recordAOTICs || JitOptions.dumpAOTICs);
  if (!code || !stubInfo) return true;

  uint32_t numFields = 0;
  while (stubInfo->fieldType(numFields) != StubField::Type::Limit) {
    numFields++;
  }
  const uint8_t* cacheIRBytes = stubInfo->code();
  uint32_t cacheIRLen = stubInfo->codeLength();
  const uint8_t* fieldTypeBytes = cacheIRBytes + cacheIRLen;

  mozilla::SHA1Sum sha;
  uint8_t kindByte = uint8_t(stubInfo->kind());
  sha.update(&kindByte, sizeof(kindByte));
  sha.update(cacheIRBytes, cacheIRLen);
  sha.update(fieldTypeBytes, numFields);
  mozilla::SHA1Sum::Hash hash;
  sha.finish(hash);

  AOTHashKey key;
  memcpy(key.bytes, hash, sizeof(hash));

  auto& accum = cx->runtime()->jitRuntime()->aotDump_;
  {
    LockGuard<Mutex> lock(accum.mutex);
    auto ptr = accum.recordedICStubHashes.lookupForAdd(key);
    if (ptr) return true;
    if (!accum.recordedICStubHashes.add(ptr, key)) return false;
  }

  AOTBlobWriter blob(AOTBlobKind::InlineCacheStub,
                     /* nameHash = */ 0,
                     "IC_" + std::to_string(accum.icStubBlobs.length()));

  AOTPayload_InlineCacheStub payload{
      .fields = {
          .kind = stubInfo->kind(),
          .makesGCCalls = uint8_t(stubInfo->makesGCCalls() ? 1 : 0),
          .stubDataOffset = uint8_t(stubInfo->stubDataOffset()),
          .localTracingSlots = uint8_t(code->localTracingSlots()),
          .cacheIRCodeLength = cacheIRLen,
          .numStubFields = numFields,
      },
      .code = mozilla::Span(code->raw(), code->instructionsSize()),
      .cacheIRCode = mozilla::Span(cacheIRBytes, cacheIRLen),
      .fieldTypes = mozilla::Span(fieldTypeBytes, size_t(numFields)),
  };

  if (!EncodeAOTBlob_InlineCacheStub(blob, payload)) return false;

  uint32_t hashPrefix;
  memcpy(&hashPrefix, hash, sizeof(hashPrefix));
  AOT_INSTR(AOTInstr_IC,
            "ic-record sha1=%08x kind=%u code=%u cacheIR=%u fields=%u\n",
            hashPrefix, unsigned(kindByte),
            unsigned(code->instructionsSize()), cacheIRLen, numFields);

  return accum.icStubBlobs.append(std::move(blob));
}

bool DumpAOTContainer(JSContext* cx) {
  MOZ_ASSERT(JitOptions.dumpAOTBlinterp ||
             JitOptions.dumpAOTSelfHosted ||
             JitOptions.dumpAOTICs ||
             JitOptions.dumpAOTBaselineCorpus ||
             JitOptions.recordAOTBaselineCorpus ||
             JitOptions.recordAOTICs);

  const char* textPath = getenv("JS_AOT_TEXT_BIN");
  if (!textPath) textPath = kAOTTextBinDefault;
  const char* containerPath = getenv("JS_AOT_CONTAINER_BIN");
  if (!containerPath) containerPath = kAOTContainerBinDefault;
  AOTContainerWriter container;
  auto& accum = cx->runtime()->jitRuntime()->aotDump_;

  if (accum.interpreterBlob) {
    if (!container.addBlob(std::move(*accum.interpreterBlob))) return false;
    accum.interpreterBlob.reset();
  }

  if (JitOptions.dumpAOTSelfHosted) {
    if (!cx->realm()) {
      JitSpew(JitSpew_BaselineAOT,
              "Skipping self-hosted dump: no realm available");
    } else {

    JS::RootedVector<JSAtom*> names(cx);
    {
      auto& map = cx->runtime()->selfHostScriptMap.ref();
      if (!names.reserve(map.count())) return false;
      for (auto iter = map.iter(); !iter.done(); iter.next()) {
        names.infallibleAppend(iter.get().key());
      }
    }

    uint32_t compiled = 0;
    uint32_t skipped = 0;
    for (JSAtom* rawAtom : names.get()) {
      Rooted<JSAtom*> atom(cx, rawAtom);
      RootedScript script(cx);
      if (!DriveSelfHostedBaselineForAOT(cx, atom, &script)) {
        skipped++;
        continue;
      }
      if (!RecordAOTBaselineFunction(cx, script)) return false;
      compiled++;
    }
    JitSpew(JitSpew_BaselineAOT,
            "Self-hosted AOT: compiled %u, skipped %u", compiled, skipped);
    }
  }

  for (auto& icBlob : accum.icStubBlobs) {
    if (!container.addBlob(std::move(icBlob))) return false;
  }
  accum.icStubBlobs.clearAndFree();

  {
    uint32_t inMemCount = accum.baselineFunctionBlobs.length();
    for (auto& blob : accum.baselineFunctionBlobs) {
      if (!container.addBlob(std::move(blob))) return false;
    }
    accum.baselineFunctionBlobs.clearAndFree();

    if (inMemCount > 0) {
      JitSpew(JitSpew_BaselineAOT, "Baseline corpus: in-memory=%u",
              inMemCount);
    }
  }

  if (container.blobCount() == 0) {
    JitSpew(JitSpew_BaselineAOT, "No blobs to write, skipping container.");
    return true;
  }

  std::ofstream textOut(textPath, std::ios::trunc | std::ios::binary);
  if (!textOut.is_open()) {
    JitSpew(JitSpew_BaselineAOT, "Failed to open %s for writing.", textPath);
    return false;
  }
  std::ofstream containerOut(containerPath,
                             std::ios::trunc | std::ios::binary);
  if (!containerOut.is_open()) {
    JitSpew(JitSpew_BaselineAOT, "Failed to open %s for writing.",
            containerPath);
    return false;
  }

  if (!container.finalize(textOut, containerOut)) {
    return false;
  }

  textOut.close();
  containerOut.close();

  JitSpew(JitSpew_BaselineAOT,
          "Wrote AOT container with %u blob(s): text=%s container=%s",
          container.blobCount(), textPath, containerPath);
  JitSpew(JitSpew_BaselineAOT, "Rebuild the engine to use AOT mode.");

  return true;
}

// ============================================================
// Load Entry Points
// ============================================================

bool LoadAOTInterpFromContainer(JSContext* cx,
                                BaselineInterpreter& interpreter) {
  AOT_TIMER_BEGIN(interp);

  auto container = AOTContainerReader::fromEmbedded();
  if (!container) {
    JitSpew(JitSpew_BaselineAOT,
            "ERROR: No valid AOT container embedded");
    return false;
  }

  auto reader = container->getBlob(AOTBlobKind::BaselineInterpreter);
  if (!reader) {
    JitSpew(JitSpew_BaselineAOT,
            "ERROR: No BaselineInterpreter blob in AOT container!");
    return false;
  }

  AOTPayload_BaselineInterpreter payload;
  if (!DecodeAOTBlob_BaselineInterpreter(*reader, &payload)) {
    JitSpew(JitSpew_BaselineAOT,
            "ERROR: Interpreter blob decode failed (fields expected %zu, "
            "got %u). Stale AOT container?",
            sizeof(AOTFields_BaselineInterpreter),
            reader->entry()->fieldsSize);
    return false;
  }

  JitCode* code = AllocateAOTCode(
      cx, reader->entry(), GetAOTTextBase(), CodeKind::Other);
  if (!code) {
    return false;
  }

  if (!interpreter.initFromAOT(cx, code, payload)) {
    JitSpew(JitSpew_BaselineAOT,
            "ERROR: Failed to initialize from AOT symbols");
    return false;
  }

  {
    auto profEntry = MakeJitcodeGlobalEntry<BaselineInterpreterEntry>(
        cx, code, code->raw(), code->rawEnd());
    if (!profEntry) {
      return false;
    }

    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(std::move(profEntry))) {
      ReportOutOfMemory(cx);
      return false;
    }

    code->setHasBytecodeMap();
  }

  if (cx->runtime()->geckoProfiler().enabled()) {
    interpreter.toggleProfilerInstrumentation(true);
  }

  if (coverage::IsLCovEnabled()) {
    interpreter.toggleCodeCoverageInstrumentationUnchecked(true);
  }

  AOT_TIMER_END(interp, "aot-load", "interp", " bytes=%u",
                reader->entry()->codeSize);

  AOT_INSTR(AOTInstr_Lifecycle,
            "jit-compile tier=blinterp bytes=%u aot=1\n",
            reader->entry()->codeSize);

  return true;
}

bool LoadAOTBaselineFunction(JSContext* cx, HandleScript script) {
  if (script->isDebuggee()) return false;

  auto container = AOTContainerReader::fromEmbedded();
  if (!container) return false;

  uint32_t probe = ComputeBaselineProbeHash(script);
  if (!container->hasBaselineProbe(probe)) {
    return false;
  }

  mozilla::SHA1Sum::Hash liveHash;
  HashBaselineIdentity(script, liveHash);

  enum class InstallResult { NoMatch, Installed, Failed };
  InstallResult result = InstallResult::NoMatch;

  container->forEachBlobWithHash(
      AOTBlobKind::BaselineFunction, probe,
      [&](AOTBlobReader& reader) -> bool {
        AOTPayload_BaselineFunction payload;
        if (!DecodeAOTBlob_BaselineFunction(reader, &payload)) {
          JitSpew(JitSpew_BaselineAOT,
                  "AOT baseline function fields size mismatch probe=%u",
                  probe);
          return false;
        }

        if (memcmp(payload.fields.identityHash, liveHash,
                   mozilla::SHA1Sum::kHashSize) != 0) {
          JitSpew(JitSpew_BaselineAOT,
                  "AOT baseline function identity hash mismatch probe=%u",
                  probe);
          return false;
        }

        JitSpew(JitSpew_BaselineAOT,
                "AOT baseline function HIT probe=%u codeSize=%u script=%s:%u",
                probe, reader.entry()->codeSize, script->filename(),
                script->lineno());

        result = InstallBaselineScriptPayload(cx, script, reader.entry(),
                                              payload)
                     ? InstallResult::Installed
                     : InstallResult::Failed;
        return true;
      });

  return result == InstallResult::Installed;
}

bool LoadAOTICStubs(JSContext* cx) {
  MOZ_ASSERT(cx->inAtomsZone());
  AOT_TIMER_BEGIN(ics);

  JitZone* jitZone = cx->zone()->jitZone();
  if (!jitZone) {
    return false;
  }

  auto container = AOTContainerReader::fromEmbedded();
  if (!container) {
    return false;
  }

  uint8_t* textBase = GetAOTTextBase();
  uint32_t loadedCount = 0;
  uint32_t totalCount = 0;

  container->forEachBlob(AOTBlobKind::InlineCacheStub,
      [&](AOTBlobReader& reader) {
    totalCount++;
    AOTPayload_InlineCacheStub payload;
    if (!DecodeAOTBlob_InlineCacheStub(reader, &payload)) {
      return;
    }
    const auto& fields = payload.fields;

    CacheIRStubInfo* stubInfo = CacheIRStubInfo::NewFromSerialized(
        fields.kind, ICStubEngine::Baseline,
        fields.makesGCCalls != 0,
        fields.stubDataOffset,
        payload.cacheIRCode.data(), payload.cacheIRCode.size(),
        payload.fieldTypes.data(), payload.fieldTypes.size());
    if (!stubInfo) {
      return;
    }

    JitCode* code = AllocateAOTCode(
        cx, reader.entry(), textBase, CodeKind::Baseline);
    if (!code) {
      js_free(stubInfo);
      return;
    }

    code->setLocalTracingSlots(fields.localTracingSlots);

    CacheIRStubKey::Lookup lookup(
        fields.kind, ICStubEngine::Baseline,
        stubInfo->code(), stubInfo->codeLength());

    CacheIRStubInfo* existing = nullptr;
    if (jitZone->getBaselineCacheIRStubCode(lookup, &existing)) {
      js_free(stubInfo);
      return;
    }

    CacheIRStubKey key(stubInfo);
    if (!jitZone->putBaselineCacheIRStubCode(lookup, key, code)) {
      return;
    }

    loadedCount++;
  });

  if (loadedCount > 0) {
    JitSpew(JitSpew_BaselineAOT,
            "Loaded %u/%u AOT IC stubs from container",
            loadedCount, totalCount);
  }
  if (loadedCount < totalCount) {
    JitSpew(JitSpew_BaselineAOT,
            "WARNING: %u AOT IC stubs failed to load",
            totalCount - loadedCount);
  }

  AOT_TIMER_END(ics, "aot-load", "ics", " count=%u", loadedCount);

  return loadedCount > 0;
}

#endif  // ENABLE_JS_AOT

}  // namespace js::jit
