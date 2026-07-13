/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"

#include "mozilla/HashFunctions.h"
#include "mozilla/Sprintf.h"

#include <cstdint>
#include <cstring>
#include <fstream>

#include "frontend/CompilationStencil.h"
#include "gc/Zone.h"
#include "jit/AOT.h"
#include "jit/AOTInstrumentation.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRSpewer.h"
#include "jit/CacheIRWriter.h"
#include "jit/IonTypes.h"
#include "jit/JitCode.h"
#include "jit/JitcodeMap.h"
#include "jit/JitContext.h"
#include "jit/JitHints.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/ProcessExecutableMemory.h"
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

void DumpAOTICStubToDir(const char* dir, CacheKind kind,
                        const CacheIRWriter& writer) {
  CacheIRStubKey::Lookup lookup(kind, ICStubEngine::Baseline,
                                writer.codeStart(), writer.codeLength());
  HashNumber h = CacheIRStubKey::hash(lookup);

  char filename[600];
  SprintfLiteral(filename, "%s/IC-%u", dir, unsigned(h));

  FILE* f = fopen(filename, "w");
  if (!f) {
    fprintf(stderr, "DumpAOTICStubToDir: fopen %s failed: %s\n", filename,
            strerror(errno));
    return;
  }
  {
    Fprinter printer(f);
    SpewCacheIROpsAsAOT(printer, kind, writer);
  }
  fflush(f);
  fclose(f);
}

void MaybeDumpICStubForPGO(CacheKind kind, const CacheIRWriter& writer,
                           bool isAOTFill) {
  if (isAOTFill || !gAOTInstr.enabled(AOTInstr_IC) ||
      !gAOTInstr.pgoDumpDir) {
    return;
  }
  DumpAOTICStubToDir(gAOTInstr.pgoDumpDir, kind, writer);
}

// Magic prefix on the per-blob canonical byte string. Distinct from
// AOT_CONTAINER_MAGIC so a truncated read of canonical bytes cannot be
// confused with a container header.
static constexpr uint32_t kBaselineCanonicalMagic = 0x424C4E63;  // 'BLNc'

static bool ComputeBaselineCanonical(
    JSScript* script,
    Vector<uint8_t, 0, SystemAllocPolicy>& out) {
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
      script->function()
          ? uint32_t(script->function()->flags().toRaw())
          : 0u;
  uint16_t nargs =
      script->function() ? uint16_t(script->function()->nargs())
                         : uint16_t(0);
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

// Serialize a live BaselineScript into a BaselineFunction blob.
// Owns the resume-offset buffer and payload internally; caller only
// supplies the canonical bytes (which come from ComputeBaselineCanonical).
[[nodiscard]] static bool EncodeBaselineFunctionBlob(
    HandleScript script, mozilla::Span<const uint8_t> canonical,
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
          .canonicalSize = uint32_t(canonical.size()),
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
      .canonical = canonical,
  };

  return EncodeAOTBlob_BaselineFunction(blob, payload);
}

// Install a decoded BaselineFunction payload on the given JSScript.
// Allocates JitCode, creates BaselineScript, copies arrays, generates
// the AOT preamble, registers in JitcodeGlobalTable, and toggles the
// profiler. Used by both the self-hosted delazify hook and the guest
// baseline compile hook.
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

[[nodiscard]] static bool GenerateAOTPreambleFor(JSContext* cx, JitCode* code) {
  mozilla::Maybe<JitContext> jctx;
  if (!MaybeGetJitContext()) {
    jctx.emplace(cx);
  }
  JitRuntime* jrt = cx->runtime()->jitRuntime();
  JitCode* preamble =
      jrt->generateAOTPreamble(cx, code->raw(), AOTSelfHostedPassReg);
  if (!preamble) return false;
  if (!jrt->aotPreambles_.append(
          JitRuntime::AOTPreambleEntry{code->raw(), preamble})) {
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

[[nodiscard]] static bool InstallBaselineScriptPayload(
    JSContext* cx, HandleScript script,
    const AOTBlobDirectoryEntry* entry,
    const AOTPayload_BaselineFunction& payload) {
  JitCode* code =
      AllocateAOTCode(cx, entry, GetAOTTextBase(), CodeKind::Baseline);
  if (!code) return false;

  BaselineScript* bs = NewAOTBaselineScript(cx, code, payload);
  if (!bs) return false;

  if (!GenerateAOTPreambleFor(cx, code)) return false;

  script->jitScript()->setBaselineScript(script, bs);

  if (!RegisterAOTBaselineForProfiler(cx, script, code)) return false;

  MaybeToggleProfilerForAOTBaseline(cx, bs);
  return true;
}

bool BuildAndSaveInterpBlob(JSContext* cx,
                            const AOTPayload_BaselineInterpreter& payload) {
  auto& saved = cx->runtime()->jitRuntime()->aotDump_.interpreterBlob;
  saved.reset();
  saved.emplace(AOTBlobKind::BaselineInterpreter,
                /* nameHash = */ 0, kNoCorpusIndex, "BaselineInterpreter");

  if (!EncodeAOTBlob_BaselineInterpreter(*saved, payload)) {
    return false;
  }

  JitSpew(JitSpew_BaselineAOT,
          "Saved interpreter blob: code=%zu fields=%zu",
          payload.code.size(),
          sizeof(AOTFields_BaselineInterpreter));
  return true;
}

static bool compileAOTSelfHosted(JSContext* cx, Handle<JSAtom*> atom,
                                 AOTBlobWriter* blobOut) {
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

  BaselineScript* bs = script->baselineScript();

  Vector<uint8_t, 0, SystemAllocPolicy> canonical;
  if (!ComputeBaselineCanonical(script, canonical)) return false;
  mozilla::Span<const uint8_t> canonicalSpan(canonical.begin(),
                                             canonical.length());

  blobOut->setNameHash(uint32_t(mozilla::HashBytes(
      canonicalSpan.data(), canonicalSpan.size())));

  if (!EncodeBaselineFunctionBlob(script, canonicalSpan, bs, *blobOut)) {
    return false;
  }

  JitSpew(JitSpew_BaselineAOT,
          "AOT Compiled '%-22s' size=%5zub  callSites=%zu  osr=%zu",
          nameStr.get(), bs->method()->instructionsSize(),
          bs->aotRetAddrEntries().size(), bs->aotOSREntries().size());
  return true;
}

bool RecordAOTBaselineFunction(JSContext* cx, HandleScript script) {
  MOZ_ASSERT(JitOptions.dumpAOTBaselineCorpus);
  MOZ_ASSERT(script->hasBaselineScript());

  BaselineScript* bs = script->baselineScript();

  Vector<uint8_t, 0, SystemAllocPolicy> canonical;
  if (!ComputeBaselineCanonical(script, canonical)) {
    return false;
  }
  mozilla::Span<const uint8_t> canonicalSpan(canonical.begin(),
                                             canonical.length());
  uint32_t key = uint32_t(
      mozilla::HashBytes(canonicalSpan.data(), canonicalSpan.size()));

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
    name = "<top-level>";
  }

  AOTBlobWriter blob(AOTBlobKind::BaselineFunction, key, kNoCorpusIndex,
                     std::move(name));

  if (!EncodeBaselineFunctionBlob(script, canonicalSpan, bs, blob)) {
    return false;
  }

  JitSpew(JitSpew_BaselineAOT,
          "AOT baseline corpus recorded key=%u size=%zu canonical=%zu "
          "nargs=%u scope=%u '%s'",
          key, bs->method()->instructionsSize(), canonicalSpan.size(),
          script->function() ? unsigned(script->function()->nargs()) : 0u,
          unsigned(script->outermostScope()->kind()),
          blob.name().c_str());

  auto& accum = cx->runtime()->jitRuntime()->aotDump_;
  if (!accum.baselineFunctionBlobs.append(std::move(blob))) return false;
  return true;
}

bool DumpAOTContainer(JSContext* cx) {
  MOZ_ASSERT(JitOptions.dumpAOTBaseline ||
             JitOptions.dumpAOTSelfHosted ||
             JitOptions.dumpAOTICs ||
             JitOptions.dumpAOTBaselineCorpus);

  const char* outPath = kAOTOutputPath;
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
      UniqueChars nameStr = AtomToPrintableString(cx, atom);
      if (!nameStr) {
        skipped++;
        continue;
      }

      AOTBlobWriter blob(AOTBlobKind::BaselineFunction,
                         /* nameHash = */ 0, kNoCorpusIndex,
                         std::string(nameStr.get()));
      if (!compileAOTSelfHosted(cx, atom, &blob)) {
        skipped++;
        continue;
      }
      compiled++;
      if (!container.addBlob(std::move(blob))) return false;
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
    uint32_t corpusCount = accum.baselineFunctionBlobs.length();
    for (auto& blob : accum.baselineFunctionBlobs) {
      if (!container.addBlob(std::move(blob))) return false;
    }
    accum.baselineFunctionBlobs.clearAndFree();
    if (corpusCount > 0) {
      JitSpew(JitSpew_BaselineAOT,
              "Baseline corpus: %u blob(s) added to container", corpusCount);
    }
  }

  if (container.blobCount() == 0) {
    JitSpew(JitSpew_BaselineAOT, "No blobs to write, skipping container.");
    return true;
  }

  std::ofstream out(outPath, std::ios::trunc);
  if (!out.is_open()) {
    JitSpew(JitSpew_BaselineAOT, "Failed to open %s for writing.", outPath);
    return false;
  }

  if (!container.finalize(out)) {
    return false;
  }

  out.close();

  JitSpew(JitSpew_BaselineAOT, "Wrote AOT container with %u blob(s) to %s",
          container.blobCount(), outPath);
  JitSpew(JitSpew_BaselineAOT, "Rebuild the engine to use AOT mode.");

  return true;
}

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

  if (!cx->runtime()->jitRuntime()->ensureDebugTrapHandler(
          cx, DebugTrapHandlerKind::Interpreter)) {
    return false;
  }
  if (!cx->runtime()->jitRuntime()->ensureDebugTrapHandler(
          cx, DebugTrapHandlerKind::Compiler)) {
    return false;
  }

  AOTIndirectionTable& table = cx->runtime()->jitRuntime()->aotIndirectionTable();
  table.set(AOTSlot::DebugTrapInterpreter,
            uintptr_t(cx->runtime()->jitRuntime()->debugTrapHandler(
                DebugTrapHandlerKind::Interpreter)->raw()));
  table.set(AOTSlot::DebugTrapCompiler,
            uintptr_t(cx->runtime()->jitRuntime()->debugTrapHandler(
                DebugTrapHandlerKind::Compiler)->raw()));

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

  Vector<uint8_t, 0, SystemAllocPolicy> canonical;
  if (!ComputeBaselineCanonical(script, canonical)) return false;
  uint32_t key =
      uint32_t(mozilla::HashBytes(canonical.begin(), canonical.length()));

  bool installed = false;
  bool failed = false;

  container->forEachBlobWithHash(
      AOTBlobKind::BaselineFunction, key,
      [&](AOTBlobReader& reader) -> bool {
        AOTPayload_BaselineFunction payload;
        if (!DecodeAOTBlob_BaselineFunction(reader, &payload)) {
          JitSpew(JitSpew_BaselineAOT,
                  "AOT baseline function fields size mismatch key=%u", key);
          return false;
        }

        if (payload.canonical.size() != canonical.length() ||
            memcmp(payload.canonical.data(), canonical.begin(),
                   canonical.length()) != 0) {
          JitSpew(JitSpew_BaselineAOT,
                  "AOT baseline function canonical mismatch key=%u "
                  "(stored=%zu live=%zu)",
                  key, payload.canonical.size(), canonical.length());
          return false;
        }

        JitSpew(JitSpew_BaselineAOT,
                "AOT baseline function HIT key=%u codeSize=%u script=%s:%u",
                key, reader.entry()->codeSize, script->filename(),
                script->lineno());

        if (!InstallBaselineScriptPayload(cx, script, reader.entry(),
                                          payload)) {
          failed = true;
        } else {
          installed = true;
        }
        return true;
      });

  if (failed) return false;
  return installed;
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

    // Register the (JitCode, CacheIRStubInfo) pair against its corpus index
    // so eager-attach hints in initICEntries() can look it up. stubInfo was
    // moved into the primary map above; re-fetch the canonical pointer.
    uint32_t corpusIdx = reader.entry()->corpusIndex;
    CacheIRStubInfo* installed = nullptr;
    (void)jitZone->getBaselineCacheIRStubCode(lookup, &installed);
    MOZ_ASSERT(installed);
    if (corpusIdx == kNoCorpusIndex ||
        !jitZone->setAOTStubEntry(corpusIdx, code, installed)) {
      // Non-fatal: the primary code map still works; only hints for this
      // stub become no-ops.
      JitSpew(JitSpew_BaselineAOT,
              "AOTStubEntry insert failed for idx=%u", corpusIdx);
    }

    AOT_INSTR(AOTInstr_IC, "ic-corpus kind=%s hash=%u code=%u idx=%u\n",
              CacheKindNames[uint8_t(fields.kind)],
              unsigned(CacheIRStubKey::hash(lookup)),
              unsigned(code->instructionsSize()),
              corpusIdx);

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
