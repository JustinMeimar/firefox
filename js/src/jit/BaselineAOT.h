/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>
#include "CompileWrappers.h"

namespace js::jit {

extern "C" {
  extern const uint8_t baseline_blob_start[];
  extern const uint8_t baseline_blob_end[];
}

inline uint8_t* GetAOTBaselineBlob() {
  return const_cast<uint8_t*>(baseline_blob_start);
}

inline std::size_t GetAOTBaselineSize() {
  return baseline_blob_end - baseline_blob_start;
}

/* This baseline metadata is used at load time to reconstruct the
 * BaslineInterpreter class, which needs all the offsets as data members.
 * I think there must be a more elegant way to encode this information,
 * currently we lay out the counts and vectors in a sensitive order:
 *
 * ----------------------------------
 *  VEC_1_COUNT, VEC_2_COUNT, ....
 * ----------------------------------
 *  VEC_1_1, ... VEC_1_N
 *  VEC_2_1, ... VEC_2_M
 *  ...
 * ----------------------------------
 * */
enum class BaselineMetadataID : uint32_t {
  InterpretOp = 0,
  InterpretOpNoDebugTrap,
  BailoutPrologue,
  ProfilerEnterToggle,
  ProfilerExitToggle,
  DebugTrapHandler,
  DispatchTableOffset,
  CallVMDebugPrologue,
  CallVMDebugEpilogue,
  CallVMDebugAfterYield,

  // Counts for vectors
  DebugInstrumentationCount,
  DebugTrapCount,
  CodeCoverageCount,
  ICReturnCount,
  PatchCount,
  OpHandlerOffsetCount,
  Count
};

class CompileRuntime;
using PatchHandlerID = uint32_t;

/* 
 * */
struct PatchContext {
    uint8_t* codeBase;
    const uint32_t* opHandlerOffsets;
};


/* Patch embedded into the binary. Associates an offset with a
 * Resolve function to patch with through the PatchHandlerID,
 * which effectively acts like a Key. A payload may also be 
 * associated to parameterize the Resolve call, though this is
 * still unclear, as only one Patch type uses it for the dispatch
 * table (to parameterize which op handler to patch.)
 * */
struct alignas(8) PatchEntry {
    uint32_t offset;
    PatchHandlerID type;
    uintptr_t payload;
};

using PatchResolverFn = uintptr_t (*)(const PatchContext& ctx, uintptr_t payload);

class PatchRegistry {
public:
    static void Register(PatchHandlerID id, PatchResolverFn fn);
    static uintptr_t Resolve(PatchHandlerID id, const PatchContext& ctx, uintptr_t payload);
};


// Patch type 1: Used in `BaselineInterpreterGenerator::emitInterpreterLoop`
struct DispatchTablePatch {
    static constexpr PatchHandlerID ID = 102;
    static uintptr_t Resolve(const PatchContext& ctx, uintptr_t payload) {
        uint32_t offset = ctx.opHandlerOffsets[payload];
        return uintptr_t(ctx.codeBase + offset);
    }
};

// TODO: These inits can be macro'd out once stable.
void InitBaselinePatches();

static const uint32_t AOT_BASELINE_FOOTER_MAGIC = 0x424C494E;

struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_BASELINE_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1;
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

/** */
struct alignas(4) BaselineManifest {
  uint32_t metadata[uint32_t(BaselineMetadataID::Count)];
  // uint32_t debugInstrumentation[DebugInstrumentationCount]
  // uint32_t debugTraps[DebugTrapCount]
  // uint32_t codeCoverage[CodeCoverageCount]
  // ICReturnOffsetEntry icReturns[ICReturnCount]
  // PatchEntry patches[PatchCount]
};

struct alignas(4) ICReturnOffsetEntry {
  uint32_t offset;
  uint32_t opcode;
};

static_assert(sizeof(BaselineAOTFooter) == 12, "Footer must be 12 bytes");
static_assert(sizeof(BaselineManifest) ==
    static_cast<uint32_t>(BaselineMetadataID::Count) * 4, "Manifest must be 68 bytes (17 fields × 4)");
static_assert(sizeof(ICReturnOffsetEntry) == 8, "ICReturnOffsetEntry must be 8 bytes");
static_assert(sizeof(PatchEntry) == 16, "PatchEntry must be 16 bytes");

}  // namespace js::jit


#endif  // jit_BaselineAOT_h
