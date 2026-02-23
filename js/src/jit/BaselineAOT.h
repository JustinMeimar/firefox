/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>
#include "jit/VMFunctions.h"
#include "vm/JSContext.h"

namespace js::jit {
enum class DebugTrapHandlerKind;

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

extern "C" {
  extern const uint8_t bl_aot_code_start[];
  extern const uint8_t bl_aot_code_end[];

#define DECLARE_MANIFEST_EXTERN(name) extern const uint32_t bl_aot_##name;
  BASELINE_MANIFEST_FIELDS(DECLARE_MANIFEST_EXTERN)
#undef DECLARE_MANIFEST_EXTERN
  
  extern const uint8_t bl_aot_DebugInstrumentationOffsets_start[];
  extern const uint8_t bl_aot_DebugInstrumentationOffsets_end[];
  extern const uint8_t bl_aot_DebugTrapOffsets_start[];
  extern const uint8_t bl_aot_DebugTrapOffsets_end[];
  extern const uint8_t bl_aot_CodeCoverageOffsets_start[];
  extern const uint8_t bl_aot_CodeCoverageOffsets_end[];
  extern const uint8_t bl_aot_ICReturnOffsets_start[];
  extern const uint8_t bl_aot_ICReturnOffsets_end[];
  extern const uint8_t bl_aot_RuntimePatches_start[];
  extern const uint8_t bl_aot_RuntimePatches_end[];
}

inline const uint8_t* GetAOTBaselineCode() {
  return bl_aot_code_start;
}

inline size_t GetAOTBaselineCodeSize() {
  return bl_aot_code_end - bl_aot_code_start;
}

// Named scalar fields for the AOT manifest. The X-macro drives both
// .S emission (serializeAOTManifest) and loading (initFromAOT).
struct AOTManifestScalars {
#define DECLARE_FIELD(name) uint32_t name = 0;
  BASELINE_MANIFEST_FIELDS(DECLARE_FIELD)
#undef DECLARE_FIELD
};

// Load time context required to apply patches. 
struct PatchContext {
    JSContext* cx; 
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;    
};

// IDs for C++ functions called via callWithABI from the baseline interpreter.
// These are used to identify which function pointer to patch at AOT load time.
enum class AOTCppFunctionId : uint32_t {
  PostWriteBarrier,
  FrameIsDebuggeeCheck,
  HandleCodeCoverageAtPrologue,
  HandleCodeCoverageAtPC,
  Count
};

#define RUNTIME_PATCH_KINDS(V) \
  V(WellKnownSymbols)         \
  V(JitRuntime)                \
  V(ContextRealm)              \
  V(JSContextPtr)              \
  V(DispatchTable)             \
  V(VMWrapper)                 \
  V(InterruptBits)             \
  V(JitActivation)             \
  V(RealmPtr)                  \
  V(LastBufferedCell)          \
  V(ProfilerEnabled)           \
  V(DebugTrapHandler)          \
  V(ProfilerExitFrameTail)     \
  V(CppFunction)               \
  V(DoubleToInt32Stub)

class RuntimePatch {
  public:   
    // Each patch has a kind tag, telling us which kind of patch to apply, and a
    // targetOffset, representing at which byte we should apply the patch.
    enum class Kind : uint16_t {
#define EMIT_KIND(name) name,
      RUNTIME_PATCH_KINDS(EMIT_KIND)
#undef EMIT_KIND
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
    uintptr_t getValueToPatch(const PatchContext& pc) const;
};

static constexpr uintptr_t AOT_PATCH_SENTINEL = 0x0000A070DEADBEEF;

static_assert(sizeof(RuntimePatch) == 12,
              "RuntimePatch size must be 12 bytes");

}  // namespace js::jit

#endif  // jit_BaselineAOT_h

