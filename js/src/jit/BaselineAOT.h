/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

// Baseline-specific AOT manifests, dump/load functions, and the
// self-hosted function list. Generic AOT infrastructure (container
// format, context) lives in AOT.h.

#include "mozilla/Span.h"

#include <string>

#include "jit/AOT.h"
#include "jit/CacheIR.h"
#include "jit/ICProfiling.h"
#include "js/Vector.h"

namespace js::jit {

class BaselineInterpreter;

struct AOTBlobData {
  AOTBlobDirectoryEntry dirEntry;
  std::string name;
  Vector<uint8_t, 0, SystemAllocPolicy> code;
  Vector<uint8_t, 0, SystemAllocPolicy> manifest;
  Vector<uint8_t, 0, SystemAllocPolicy> metadata;

  template <typename T>
  static bool appendBytes(Vector<uint8_t, 0, SystemAllocPolicy>& vec,
                          const T* data, size_t count) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    return vec.append(bytes, count * sizeof(T));
  }
};

static constexpr const char* kAOTOutputPath =
    "js/src/jit/AOTBaseline.S";

// [SMDOC] AOT Baseline Interpreter Manifest
//
// Fixed-size header serialized alongside the interpreter code blob.  Each
// field records a byte offset (or count) that the load-time code needs to
// reconstruct the BaselineInterpreter state: opcode handler entry points,
// profiler/debug toggle sites, and the lengths of the variable-length
// metadata arrays that follow the manifest in the blob.
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

// Helpers for reading/writing packed metadata arrays.
// Metadata regions are sequences of typed arrays laid end-to-end.
// These eliminate manual cursor arithmetic on both dump and load sides.

template<typename T>
inline mozilla::Span<const T> ReadMetadataArray(const uint8_t*& cursor,
                                                uint32_t count) {
  auto span = mozilla::Span(reinterpret_cast<const T*>(cursor), count);
  cursor += count * sizeof(T);
  return span;
}

template<typename T>
inline bool WriteMetadataArray(Vector<uint8_t, 0, SystemAllocPolicy>& vec,
                               mozilla::Span<T> data) {
  if (data.empty()) return true;
  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  return vec.append(bytes, data.size() * sizeof(T));
}

// Build and save the interpreter AOT blob to the saved-blob slot.
// Called from BaselineInterpreterGenerator::dumpAOTInterp with all
// the data extracted from the generator.  The metadata byte vectors
// are pre-packed by the caller (debugInstr, debugTraps, coverage,
// icReturns concatenated).
[[nodiscard]] bool BuildAndSaveInterpBlob(
    JitCode* code, const AOTInterpManifest& scalars,
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
[[nodiscard]] bool DumpAOTContainer(JSContext* cx,
                                    ICProfileMap* pgoProfiles = nullptr);

// Load pre-compiled IC stubs from the AOT container into the atoms JitZone.
// Returns true if stubs were loaded, false if none found or on error.
[[nodiscard]] bool LoadAOTICStubs(JSContext* cx);

mozilla::Span<const AOTBlobData> GetSavedICBlobs();
void ClearSavedICBlobs();

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
