/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>
#include "vm/JSContext.h"
#include "jit/VMFunctions.h"

namespace js::jit {

// Forward declarations
enum class DebugTrapHandlerKind;

// IDs for C++ functions called via callWithABI from the baseline interpreter.
// These are used to identify which function pointer to patch at AOT load time.
enum class AOTCppFunctionId : uint32_t {
  PostWriteBarrier,
  FrameIsDebuggeeCheck,
  HandleCodeCoverageAtPrologue,
  HandleCodeCoverageAtPC,
  Count
};

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
  RuntimePatchCount,

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
class RuntimePatch {
  public:
    enum class Kind : uint16_t {
      WellKnownSymbols,
      JitRuntime,
      ContextRealm,
      JSContextPtr,
      DispatchTable,
      VMWrapper,
      InterruptBits,
      JitActivation,
      RealmPtr,
      LastBufferedCell,
      ProfilerEnabled,
      DebugTrapHandler,
      ProfilerExitFrameTail,
      CppFunction
    };

    Kind kind;
    uint32_t targetOffset;

    union {
      uint32_t handlerOffset;
      VMFunctionId vmId;
      DebugTrapHandlerKind dbgKind;
      AOTCppFunctionId cppFnId;
    };

    static RuntimePatch DispatchTablePatch(uint32_t targetOffset_, uint32_t handlerOffset_) {
      RuntimePatch p;
      p.kind = Kind::DispatchTable;
      p.targetOffset = targetOffset_;
      p.handlerOffset = handlerOffset_;
      return p;
    }

    static RuntimePatch VMWrapperPatch(uint32_t targetOffset_, VMFunctionId vmId_) {
      RuntimePatch p;
      p.kind = Kind::VMWrapper;
      p.targetOffset = targetOffset_;
      p.vmId = vmId_;
      return p;
    }

    static RuntimePatch DebugTrapPatch(uint32_t targetOffset_, DebugTrapHandlerKind dbgKind_) {
      RuntimePatch p;
      p.kind = Kind::DebugTrapHandler;
      p.targetOffset = targetOffset_;
      p.dbgKind = dbgKind_;
      return p;
    }

    static RuntimePatch CppFunctionPatch(uint32_t targetOffset_, AOTCppFunctionId fnId) {
      RuntimePatch p;
      p.kind = Kind::CppFunction;
      p.targetOffset = targetOffset_;
      p.cppFnId = fnId;
      return p;
    }

    explicit RuntimePatch(Kind kind_, uint32_t targetOffset_) :
      kind(kind_), targetOffset(targetOffset_) {}

    void apply(const PatchContext& pc) const;

  private:
    RuntimePatch() = default;
    uintptr_t _getValueToPatch(const PatchContext& pc) const;
};

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
  uint32_t RuntimePatchCount;

  // Followed by variable-length arrays:
  // uint32_t debugInstrumentation[DebugInstrumentationCount]
  // uint32_t debugTraps[DebugTrapCount]
  // uint32_t codeCoverage[CodeCoverageCount]
  // ICReturnOffsetEntry icReturns[ICReturnCount]
  // RuntimePatch patches[RuntimePatchCount]
};

struct alignas(4) ICReturnOffsetEntry {
  uint32_t offset;
  uint32_t opcode;
};

static_assert(sizeof(BaselineAOTFooter) == 12, "Footer must be 12 bytes");
static_assert(sizeof(BaselineManifest) == static_cast<uint32_t>(BaselineMetadataID::Count) * 4,
              "Manifest size must match metadata count");
static_assert(sizeof(ICReturnOffsetEntry) == 8, "ICReturnOffsetEntry must be 8 bytes");

}  // namespace js::jit

#endif  // jit_BaselineAOT_h

