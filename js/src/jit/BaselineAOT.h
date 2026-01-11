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

struct PatchContext {
    uint8_t* codeBase;
    const uint32_t* opHandlerOffsets;
};

using PatchResolverFn = uintptr_t (*)(CompileRuntime* cx, const PatchContext& ctx, uintptr_t payload);

struct alignas(8) PatchEntry {
    uint32_t offset;
    PatchHandlerID type;
    uintptr_t payload;
};

class PatchRegistry {
public:
    static void Register(PatchHandlerID id, PatchResolverFn fn);
    static uintptr_t Resolve(PatchHandlerID id, CompileRuntime* cx, const PatchContext& ctx, uintptr_t payload);
};

struct WellKnownSymbolPatch {
    static constexpr PatchHandlerID ID = 101;
    static uintptr_t Resolve(CompileRuntime* runtime, const PatchContext&, uintptr_t) {
        return uintptr_t(&runtime->wellKnownSymbols());
    }
};

struct DispatchTablePatch {
    static constexpr PatchHandlerID ID = 102;
    static uintptr_t Resolve(CompileRuntime*, const PatchContext& ctx, uintptr_t payload) {
        uint32_t offset = ctx.opHandlerOffsets[payload];
        return uintptr_t(ctx.codeBase + offset);
    }
};

void InitBaselinePatches();

static const uint32_t AOT_BASELINE_FOOTER_MAGIC = 0x424C494E;
struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_BASELINE_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1;
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

struct alignas(4) BaselineManifest {
  uint32_t metadata[uint32_t(BaselineMetadataID::Count)];
  // Followed by:
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
static_assert(sizeof(BaselineManifest) == 64, "Manifest must be 64 bytes (16 fields × 4)");
static_assert(sizeof(ICReturnOffsetEntry) == 8, "ICReturnOffsetEntry must be 8 bytes");
// static_assert(sizeof(PatchEntry) == 12, "PatchEntry must be 12 bytes");

}  // namespace js::jit

// OLD PATCHING: (bad)
//
// enum class PatchType : uint32_t {
//   DispatchTable = 0,
//   WellKnownSymbols,
//   DebugTrapHandler,
//   VMWrapper,
// };
//
// struct alignas(4) PatchEntry {
//   uint32_t offset;
//   PatchType type;
//   uint32_t aux;
// };



#endif  // jit_BaselineAOT_h
