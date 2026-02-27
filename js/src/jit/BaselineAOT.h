/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>
#include <cstring>
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

// --- AOT Container Format ---
//
// The AOT container is a flat binary with a header, directory, and blob data.
// Two extern symbols bracket the entire container.

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  SelfHostedFunction = 1,
};

static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"
static constexpr uint32_t AOT_CONTAINER_VERSION = 1;

struct AOTContainerHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t blobCount;
  uint32_t padding;
};

static_assert(sizeof(AOTContainerHeader) == 16,
              "AOTContainerHeader must be 16 bytes");

struct AOTBlobDirectoryEntry {
  uint32_t kind;       // AOTBlobKind
  uint32_t nameHash;   // reserved for future use
  uint32_t codeOffset;
  uint32_t codeSize;
  uint32_t manifestOffset;
  uint32_t manifestSize;
  uint32_t patchesOffset;
  uint32_t patchesCount;
  uint32_t metadataOffset;
  uint32_t metadataSize;
};

static_assert(sizeof(AOTBlobDirectoryEntry) == 40,
              "AOTBlobDirectoryEntry must be 40 bytes");

extern "C" {
  extern const uint8_t bl_aot_container_start[];
  extern const uint8_t bl_aot_container_end[];
}

inline const uint8_t* GetAOTContainer() {
  return bl_aot_container_start;
}

inline size_t GetAOTContainerSize() {
  return bl_aot_container_end - bl_aot_container_start;
}

inline const AOTContainerHeader* GetAOTContainerHeader() {
  if (GetAOTContainerSize() < sizeof(AOTContainerHeader)) {
    return nullptr;
  }
  const auto* hdr = reinterpret_cast<const AOTContainerHeader*>(GetAOTContainer());
  if (hdr->magic != AOT_CONTAINER_MAGIC ||
      hdr->version != AOT_CONTAINER_VERSION) {
    return nullptr;
  }
  return hdr;
}

inline const AOTBlobDirectoryEntry* GetAOTBlobDirectory() {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return nullptr;
  return reinterpret_cast<const AOTBlobDirectoryEntry*>(
      GetAOTContainer() + sizeof(AOTContainerHeader));
}

inline const AOTBlobDirectoryEntry* FindAOTBlob(AOTBlobKind kind) {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return nullptr;
  const auto* dir = GetAOTBlobDirectory();
  for (uint32_t i = 0; i < hdr->blobCount; i++) {
    if (static_cast<AOTBlobKind>(dir[i].kind) == kind) {
      return &dir[i];
    }
  }
  return nullptr;
}

struct AOTManifestScalars {
#define DECLARE_FIELD(name) uint32_t name = 0;
  BASELINE_MANIFEST_FIELDS(DECLARE_FIELD)
#undef DECLARE_FIELD
};

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

// Simple rolling hash for AOT blob name matching.
inline uint32_t AOTNameHash(const char* s) {
  uint32_t h = 0;
  for (; *s; s++) {
    h = h * 31 + static_cast<uint8_t>(*s);
  }
  return h;
}

inline uint32_t AOTNameHash(const JS::Latin1Char* chars, size_t len) {
  uint32_t h = 0;
  for (size_t i = 0; i < len; i++) {
    h = h * 31 + chars[i];
  }
  return h;
}

inline uint32_t AOTNameHash(const char16_t* chars, size_t len) {
  uint32_t h = 0;
  for (size_t i = 0; i < len; i++) {
    h = h * 31 + static_cast<uint8_t>(chars[i]);
  }
  return h;
}

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

// Shared between ExternalRefKind and RuntimePatch::Kind.
// These must come first in both enums so ordinal values match,
// enabling static_cast between the two.
#define EXTERNAL_REF_SHARED_KINDS(V) \
  V(JSContextPtr)              \
  V(InterruptBits)             \
  V(JitActivation)             \
  V(RealmPtr)                  \
  V(ContextRealm)              \
  V(WellKnownSymbols)          \
  V(JitRuntime)                \
  V(LastBufferedCell)          \
  V(ProfilerEnabled)           \
  V(ProfilerExitFrameTail)     \
  V(DoubleToInt32Stub)

// Only in RuntimePatch::Kind (parameterized patch types).
#define RUNTIME_PATCH_ONLY_KINDS(V) \
  V(DispatchTable)             \
  V(VMWrapper)                 \
  V(DebugTrapHandler)          \
  V(CppFunction)

#define RUNTIME_PATCH_KINDS(V)  \
  EXTERNAL_REF_SHARED_KINDS(V) \
  RUNTIME_PATCH_ONLY_KINDS(V)

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

class JitCode;

// Allocate executable memory, copy code from an AOT blob, and apply runtime
// patches. Shared by loadAOTBaseline (interpreter) and
// LoadAOTSelfHostedFunction (scripts).
// |codeKind| is CodeKind::Other for interpreter, Baseline for scripts.
// |dispatchTableOffset| is nonzero only for the interpreter blob.
[[nodiscard]] JitCode* AllocateAndPatchAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    const uint8_t* containerBase, uint32_t headerSize,
    CodeKind codeKind, uint32_t dispatchTableOffset);

}  // namespace js::jit

#endif  // jit_BaselineAOT_h

