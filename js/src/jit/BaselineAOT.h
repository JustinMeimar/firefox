/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

// Baseline-specific AOT manifests, dump/load functions, and the
// self-hosted function list. Generic AOT infrastructure (container
// format, patches, accumulator, context) lives in AOT.h.

#include "jit/AOT.h"

namespace js::jit {

static constexpr const char* kAOTOutputPath =
    "js/src/jit/AOTBaselineInterpreter.S";

// =========================================================================
// Baseline interpreter manifest
// =========================================================================

#define BASELINE_MANIFEST_FIELDS(V) \
  V(InterpretOp)                    \
  V(InterpretOpNoDebugTrap)         \
  V(BailoutPrologue)                \
  V(ProfilerEnterToggle)            \
  V(ProfilerExitToggle)             \
  V(DebugTrapHandler)               \
  V(DispatchTableOffset)            \
  V(CallVMDebugPrologue)            \
  V(CallVMDebugEpilogue)            \
  V(CallVMDebugAfterYield)          \
  V(HeaderSize)                     \
  V(PrologueEndOffset)              \
  V(DebugInstrumentationCount)      \
  V(DebugTrapCount)                 \
  V(CodeCoverageCount)              \
  V(ICReturnCount)                  \
  V(RuntimePatchCount)

struct AOTManifestScalars {
#define DECLARE_FIELD(name) uint32_t name = 0;
  BASELINE_MANIFEST_FIELDS(DECLARE_FIELD)
#undef DECLARE_FIELD
};

// =========================================================================
// Baseline compilation (self-hosted) manifest
// =========================================================================

// Per-script manifest for AOT-compiled self-hosted functions.
struct AOTScriptManifest {
  uint32_t warmUpCheckPrologueOffset;
  uint32_t profilerEnterToggleOffset;
  uint32_t profilerExitToggleOffset;
  uint32_t retAddrEntryCount;
  uint32_t osrEntryCount;
  uint32_t debugTrapEntryCount;
  uint32_t resumeEntryCount;
  uint32_t codeSize;
  uint32_t headerSize;
  uint32_t runtimePatchCount;
};

// =========================================================================
// Baseline AOT dump/load functions
// =========================================================================

class BaselineInterpreter;

// Build and save the interpreter AOT blob to the saved-blob slot.
// Called from BaselineInterpreterGenerator::dumpAOTInterp with all
// the data extracted from the generator.  The metadata byte vectors
// are pre-packed by the caller (debugInstr, debugTraps, coverage,
// icReturns concatenated).
[[nodiscard]] bool BuildAndSaveInterpBlob(
    JitCode* code, const AOTManifestScalars& scalars,
    const RuntimePatchVector& patches,
    const uint8_t* metadataBytes, size_t metadataSize);

// Load the AOT interpreter blob from the embedded container and
// initialize the BaselineInterpreter.
[[nodiscard]] bool LoadAOTInterpFromContainer(
    JSContext* cx, BaselineInterpreter& interpreter);

// Load a pre-compiled self-hosted function from the AOT container.
// |name| is the self-hosted function name (used for hash matching).
// Returns true if successfully loaded, false if no blob found or on error.
[[nodiscard]] bool LoadAOTSelfHosted(JSContext* cx,
                                     HandleScript script,
                                     Handle<JSAtom*> name);

// Write the final AOT .S container (interpreter blob + self-hosted blobs).
// Must be called after a realm exists. Respects dumpBaselineInterp and
// dumpBaselineSelfHosted flags to control which blobs are included.
[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
