/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include "mozilla/Span.h"

#include <string>

#include "jit/AOT.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIR.h"
#include "js/Vector.h"

namespace js::jit {

static constexpr const char* kAOTOutputPath =
    "js/src/jit/AOTBaseline.S";

// [SMDOC] AOT Baseline Interpreter Manifest
//
// Fixed-size header serialized alongside the interpreter code blob.
struct AOTInterpManifest {
  uint32_t InterpretOp = 0;
  uint32_t InterpretOpNoDebugTrap = 0;
  uint32_t BailoutPrologue = 0;
  uint32_t ProfilerEnterToggle = 0;
  uint32_t ProfilerExitToggle = 0;
  uint32_t DebugTrapHandler = 0;
  uint32_t CallVMDebugPrologue = 0;
  uint32_t CallVMDebugEpilogue = 0;
  uint32_t CallVMDebugAfterYield = 0;
  uint32_t DebugInstrumentationCount = 0;
  uint32_t DebugTrapCount = 0;
  uint32_t CodeCoverageCount = 0;
  uint32_t ICReturnCount = 0;
};

static_assert(sizeof(AOTInterpManifest) == 52,
              "AOTInterpManifest layout changed; bump AOT_CONTAINER_MAGIC or "
              "add migration logic");

// [SMDOC] AOT Baseline Compilation (Self-Hosted) Manifest
//
// Per-script manifest for AOT-compiled self-hosted functions.
struct AOTScriptManifest {
  uint32_t warmUpCheckPrologueOffset = 0;
  uint32_t profilerEnterToggleOffset = 0;
  uint32_t profilerExitToggleOffset = 0;
  uint32_t retAddrEntryCount = 0;
  uint32_t osrEntryCount = 0;
  uint32_t debugTrapEntryCount = 0;
  uint32_t resumeEntryCount = 0;
  uint32_t codeSize = 0;
  uint32_t headerSize = 0;
};

static_assert(sizeof(AOTScriptManifest) == 36,
              "AOTScriptManifest layout changed; bump AOT_CONTAINER_MAGIC or "
              "add migration logic");

struct AOTICStubManifest {
  CacheKind kind = {};
  uint8_t makesGCCalls = 0;
  uint8_t stubDataOffset = 0;
  uint8_t localTracingSlots = 0;
  uint8_t pad = 0;
  uint32_t cacheIRCodeLength = 0;
  uint32_t numStubFields = 0;
};

static_assert(sizeof(AOTICStubManifest) == 16,
              "AOTICStubManifest layout changed; bump AOT_CONTAINER_MAGIC or "
              "add migration logic");

[[nodiscard]] bool BuildAndSaveInterpBlob(
    JitCode* code, const AOTInterpManifest& scalars,
    mozilla::Span<const uint32_t> debugInstr,
    mozilla::Span<const uint32_t> debugTraps,
    mozilla::Span<const uint32_t> coverage,
    mozilla::Span<const BaselineInterpreter::ICReturnOffset> icReturns);

[[nodiscard]] bool LoadAOTInterpFromContainer(
    JSContext* cx, BaselineInterpreter& interpreter);

[[nodiscard]] bool LoadAOTSelfHosted(JSContext* cx,
                                     HandleScript script,
                                     Handle<JSAtom*> name);

[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

[[nodiscard]] bool LoadAOTICStubs(JSContext* cx);

Vector<AOTBlobWriter, 0, SystemAllocPolicy> TakeSavedICBlobs();

class CacheIRWriter;

#ifdef ENABLE_JS_AOT

// Dump one IC stub's CacheIR body to a $AOT_ICS_DIR file so
// enforce-aot-ics runs can capture stubs that were missing from the corpus.
// No-op if $AOT_ICS_LOG_UNSEEN is unset.
void MaybeLogUnseenICStub(CacheKind kind, const CacheIRWriter& writer);

// Dump one IC stub's CacheIR body to $JS_AOT_PGO_DIR/IC-<hash> so
// SelectAOTCorpus.py can knapsack-pick the corpus from a PGO run.
// No-op if IC instrumentation is disabled or this is an AOT-fill compile.
void MaybeDumpICStubForPGO(CacheKind kind, const CacheIRWriter& writer,
                            bool isAOTFill);

#endif  // ENABLE_JS_AOT

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
