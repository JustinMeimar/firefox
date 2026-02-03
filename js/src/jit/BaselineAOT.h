/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>
#include "vm/JSContext.h"

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

// Baseline AOT metadata IDs which correspond to fields
// in the BaselineManifest struct.
enum class BaselineMetadataID : uint32_t {
  // Offset fields
  InterpretOp,
  InterpretOpNoDebugTrap,
  BailoutPrologue,
  ProfilerEnterToggle,
  ProfilerExitToggle,
  DebugTrapHandler,
  DispatchTableOffset,
  CallVMDebugPrologue,
  CallVMDebugEpilogue,
  CallVMDebugAfterYield,
  HeaderSize,
  PrologueEndOffset,

  // Count fields
  DebugInstrumentationCount,
  DebugTrapCount,
  CodeCoverageCount,
  ICReturnCount,
  TablePatchCount,
  RuntimePatchCount,
  OpHandlerOffsetCount,

  Count
};

// In order to perform the patch, the AOT loader must provide the
// code base address and the dispatch table offset.
struct PatchContext {
    JSContext* cx; 
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;    
};


// Each patch has a kind tag, telling us which kind of patch to apply, and a
// targetOffset, representing at which byte in the AOT blob we should apply
// the patch. 
struct RuntimePatch {

    enum class Kind : uint16_t {
      WellKnownSymbols,
      JitRuntime,         // JitRuntime*
      ContextRealm,       // &JSContext::realm_ field
      JSContextPtr,       // JSContext*
      DispatchTable       // uintptr_t
    };
  
    Kind kind;
    uint32_t targetOffset; 
    // Extra data used only by kind DispatchTable. Used to determine
    // which handler index to use as the value to patch. This value could
    // be computed from targetOffset - dispatchTableOffset to avoid this
    // extra field.
    uint32_t handlerOffset;
    
    RuntimePatch(Kind kind_, uint32_t targetOffset_) :
      kind(kind_), targetOffset(targetOffset_) {}
    
    RuntimePatch(uint32_t targetOffset_, uint32_t handlerOffset_)
      : kind(Kind::DispatchTable),
        targetOffset(targetOffset_),
        handlerOffset(handlerOffset_) {}

    uintptr_t _getValueToPatch(const PatchContext& pc) const {
        switch(kind) {
            case Kind::WellKnownSymbols:
                return (uintptr_t)pc.cx->runtime()->wellKnownSymbols.ref();
            case Kind::JitRuntime:
                return (uintptr_t)pc.cx->runtime()->jitRuntime();
            case Kind::ContextRealm:
                return (uintptr_t)(reinterpret_cast<const uint8_t*>(pc.cx)
                        + JSContext::offsetOfRealm());
            case Kind::JSContextPtr:
                return (uintptr_t)pc.cx;
            case Kind::DispatchTable:
                return (uintptr_t)(pc.codeBase + handlerOffset);
        }   
        MOZ_CRASH("Unexpected Patch Type");
    }

    void apply(const PatchContext& pc) const {
        uintptr_t val = _getValueToPatch(pc); 
        uint8_t* target = pc.codeBase + targetOffset;
#ifdef DEBUG   
        uintptr_t beforeValue = *reinterpret_cast<uintptr_t*>(target);
        JitSpew(JitSpew_BaselineAOT, "Runtime patch @ offset %u: before=0x%016lx after=0x%016lx",
                targetOffset, beforeValue, val);
#endif
        *reinterpret_cast<uintptr_t*>(target) = val;
      }
};

// struct alignas(8) RuntimePatch {
//   RuntimePatchId id; 
//   uint32_t targetOffset;
//   uintptr_t computePatch(const PatchContext& ctx) const;
// };
//
// struct alignas(8) DispatchTablePatch {
//     /// Offset from the blob to the start of the opcode handler (what the
//     /// patch'ed pointer points to.)
//     uint32_t handlerOffset;
//     /// Index into the dispatch table 
//     uint32_t dispatchTableIndex;
//     DispatchTablePatch(uint32_t handlerOffset_, uint32_t dispatchTableIndex_)
//       : handlerOffset(handlerOffset_), dispatchTableIndex(dispatchTableIndex_) {}
// };

// void applyPatch(const PatchContext& ctx, const DispatchTablePatch& entry);

static const uint32_t AOT_FOOTER_MAGIC = 0x424C494E;
struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1;
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

// Baseline manifest structure
// Contains metadata about the AOT-compiled baseline interpreter
struct alignas(4) BaselineManifest {
  // Offset fields
  uint32_t InterpretOp;
  uint32_t InterpretOpNoDebugTrap;
  uint32_t BailoutPrologue;
  uint32_t ProfilerEnterToggle;
  uint32_t ProfilerExitToggle;
  uint32_t DebugTrapHandler;
  uint32_t DispatchTableOffset;
  uint32_t CallVMDebugPrologue;
  uint32_t CallVMDebugEpilogue;
  uint32_t CallVMDebugAfterYield;
  uint32_t HeaderSize;
  uint32_t PrologueEndOffset;

  // Count fields
  uint32_t DebugInstrumentationCount;
  uint32_t DebugTrapCount;
  uint32_t CodeCoverageCount;
  uint32_t ICReturnCount;
  uint32_t TablePatchCount;
  uint32_t RuntimePatchCount;
  uint32_t OpHandlerOffsetCount;

  // Followed by variable-length arrays:
  // uint32_t debugInstrumentation[DebugInstrumentationCount]
  // uint32_t debugTraps[DebugTrapCount]
  // uint32_t codeCoverage[CodeCoverageCount]
  // ICReturnOffsetEntry icReturns[ICReturnCount]
  // DispatchTablePatch patches[PatchCount]
  // RuntimePatch patches[PatchCount]
  // uint32_t opHandlerOffsets[OpHandlerOffsetCount]
};

struct alignas(4) ICReturnOffsetEntry {
  uint32_t offset;
  uint32_t opcode;
};

static_assert(sizeof(BaselineAOTFooter) == 12, "Footer must be 12 bytes");
static_assert(sizeof(BaselineManifest) == static_cast<uint32_t>(BaselineMetadataID::Count) * 4,
              "Manifest size must match metadata count");
static_assert(sizeof(ICReturnOffsetEntry) == 8, "ICReturnOffsetEntry must be 8 bytes");
// static_assert(sizeof(DispatchTablePatch) == 8, "DispatchTablePatch must be 8 bytes");

}  // namespace js::jit

#endif  // jit_BaselineAOT_h

