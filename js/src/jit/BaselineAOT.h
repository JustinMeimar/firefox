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

// In order to perform the patch, the AOT loader must saturate the
// pointer to the AOT code.
struct PatchContext {
    uint8_t* codeBase;
    const uint32_t* opHandlerOffsets;
    
    PatchContext(uint8_t* codeBase_, const uint32_t* opHandlerOffsets_)
      : codeBase(codeBase_), opHandlerOffsets(opHandlerOffsets_) {}
};

// NOTE: this is just data that is already in the BaselineMetaData, so
// in theory we do not need dedicated patch entries if the only patches
// we perform are for the dispatch table. We will confirm that is the case
// however before dispensing with the patch infrastructure.

struct alignas(8) DispatchTablePatch { 
    /// Offset from the blob to the start of the opcode handler.
    /// (What the patch points to.)
    uint32_t handlerOffset;  

    /// Offset from the blob to the dipatch table entry.
    /// (The target of the patch itself).
    uint32_t handlerPtrOffset; 
};

void applyPatch(const PatchContext& ctx, const DispatchTablePatch& entry);

static const uint32_t AOT_FOOTER_MAGIC = 0x424C494E;
struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1;
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

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
static_assert(sizeof(DispatchTablePatch) == 8, "PatchEntry must be 16 bytes");

// using PatchResolverFn = uintptr_t (*)(const PatchContext& ctx, uintptr_t payload);
// class PatchRegistry {
// public:
//     static void Register(PatchResolverFn fn);
//     static uintptr_t Resolve(const PatchContext& ctx, uintptr_t payload);
// };

// struct DispatchTablePatch {
//     // static constexpr PatchHandlerID ID = 102;
//     static uintptr_t Resolve(const PatchContext& ctx, uintptr_t payload) {
//         uint32_t offset = ctx.opHandlerOffsets[payload];
//         return uintptr_t(ctx.codeBase + offset);
//     }
// };

// TODO: These inits can be macro'd out once stable.


}  // namespace js::jit


#endif  // jit_BaselineAOT_h
