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

// Variabes corresponding to global symbols in the=
// generated `.s` file.
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

// All metadata fields stored in the BaselineManifest.
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

enum class BaselineManifestField : uint32_t {
#define EMIT_ENUM(name) name,
  BASELINE_MANIFEST_FIELDS(EMIT_ENUM)
#undef EMIT_ENUM
  Count
};

// Load time context required to apply patches. 
struct PatchContext {
    JSContext* cx; 
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;    
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
  V(CppFunction)

// Each patch has a kind tag, telling us which kind of patch to apply, and a
// targetOffset, representing at which byte in the AOT blob we should apply
// the patch.
class RuntimePatch {
  public:
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
    uintptr_t _getValueToPatch(const PatchContext& pc) const;
};

static constexpr uintptr_t AOT_PATCH_SENTINEL = 0x0000A070DEADBEEF;

static const uint32_t AOT_FOOTER_MAGIC = 0x424C494E;
struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1;
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

struct alignas(4) BaselineManifest {
  uint32_t metadata[uint32_t(BaselineManifestField::Count)];
};

static_assert(sizeof(BaselineAOTFooter) == 12, "Footer must be 12 bytes");
static_assert(sizeof(BaselineManifest) ==
              static_cast<uint32_t>(BaselineManifestField::Count) * 4,
              "Manifest size must match metadata count");

}  // namespace js::jit

#endif  // jit_BaselineAOT_h

