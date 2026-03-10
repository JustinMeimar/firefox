/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h

// Generic AOT infrastructure: relocation kinds, container format,
// runtime patches, code allocation, and the AOT compilation context.
//
// This file is blob-kind-agnostic. Baseline-interpreter-specific and
// baseline-compilation-specific manifests live in BaselineAOT.h.

#include <cstdint>
#include <cstring>

#include "jstypes.h"

#include "jit/VMFunctions.h"
#include "js/Vector.h"
#include "vm/JSContext.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class MacroAssembler;

// =========================================================================
// AOT relocation kinds
// =========================================================================

// Enumerates runtime-dependent references that the JIT codegen may need.
// Each kind maps to a specific absolute address that varies
// between processes (due to ASLR, different builds, etc.).
//
// In non-AOT mode these resolve to ImmPtr at compile time. In AOT mode
// they emit movWithPatch(sentinel) and register a RuntimePatch that is
// applied at load time.

#define AOT_RELOC_KINDS(V)   \
  V(JSRuntimePtr)               \
  V(JSContextPtr)               \
  V(InterruptBits)              \
  V(JitActivation)              \
  V(RealmPtr)                   \
  V(ContextRealm)               \
  V(WellKnownSymbols)           \
  V(JitRuntime)                 \
  V(LastBufferedCell)           \
  V(ProfilerEnabled)            \
  V(ProfilerExitFrameTail)      \
  V(DoubleToInt32Stub)          \
  V(MegamorphicCache)           \
  V(MegamorphicSetPropCache)    \
  V(StringToAtomCache)          \
  V(DispatchTable)              \
  V(VMWrapper)                  \
  V(DebugTrapHandler)           \
  V(CppFunction)                \
  V(PreBarrier)                 \
  V(ExceptionTail)

enum class AOTRelocKind : uint16_t {
#define EMIT_KIND(name) name,
  AOT_RELOC_KINDS(EMIT_KIND)
#undef EMIT_KIND
  Count
};

// Map an AOTRelocKind to its string name. Usable in debug logging.
inline const char* AOTRelocKindName(AOTRelocKind kind) {
  switch (kind) {
#define EMIT_CASE(name) case AOTRelocKind::name: return #name;
    AOT_RELOC_KINDS(EMIT_CASE)
#undef EMIT_CASE
    case AOTRelocKind::Count:
      break;
  }
  return "Unknown";
}

// Resolve an AOTRelocKind to its runtime value. Used by the non-AOT path
// to get the compile-time ImmPtr value, and by the patch-based AOT path to
// verify patch correctness.
uintptr_t ResolveAOTReloc(AOTRelocKind kind, JSContext* cx);

// =========================================================================
// AOT container format
// =========================================================================

static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"
static constexpr uint32_t AOT_CONTAINER_VERSION = 1;

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  SelfHostedFunction = 1,
};

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

inline const AOTBlobDirectoryEntry* FindAOTBlob(AOTBlobKind kind,
                                                uint32_t nameHash = 0) {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return nullptr;
  const auto* dir = GetAOTBlobDirectory();
  for (uint32_t i = 0; i < hdr->blobCount; i++) {
    if (static_cast<AOTBlobKind>(dir[i].kind) == kind &&
        (nameHash == 0 || dir[i].nameHash == nameHash) &&
        dir[i].codeSize > 0) {
      return &dir[i];
    }
  }
  return nullptr;
}

template <typename CharT>
inline uint32_t AOTNameHash(const CharT* chars, size_t len) {
  uint32_t h = 0;
  for (size_t i = 0; i < len; i++) {
    h = h * 31 + static_cast<uint8_t>(chars[i]);
  }
  return h;
}
inline uint32_t AOTNameHash(const char* s) {
  return AOTNameHash(s, strlen(s));
}

// =========================================================================
// Runtime patching
// =========================================================================

// Load time context required to apply patches.
struct PatchContext {
    JSContext* cx;
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;
};

enum class DebugTrapHandlerKind;

// While trampolines are generated at runtime we must manually patch the
// `callWithABI<Fn>` calls, since the absolute address of functions in
// the text section is non-deterministic.
enum class AOTCppFunctionId : uint32_t {
  PostWriteBarrier,
  FrameIsDebuggeeCheck,
  HandleCodeCoverageAtPrologue,
  HandleCodeCoverageAtPC,
  Count
};

enum class AOTPreBarrierIndex : uint32_t {
  Value = 0,
  String,
  Object,
  Shape,
  WasmAnyRef,
  Count
};

class RuntimePatch {
  public:
    // Each patch has a kind tag, telling us which kind of patch to apply, and a
    // targetOffset, representing at which byte we should apply the patch.
    AOTRelocKind kind;
    uint32_t targetOffset;

    // Auxiliary data — interpretation depends on kind.
    uint32_t auxData;

    static RuntimePatch DispatchTablePatch(uint32_t targetOffset_,
                                           uint32_t handlerOffset_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::DispatchTable;
      p.targetOffset = targetOffset_;
      p.auxData = handlerOffset_;
      return p;
    }

    static RuntimePatch VMWrapperPatch(uint32_t targetOffset_,
                                       VMFunctionId vmId_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::VMWrapper;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(vmId_);
      return p;
    }

    static RuntimePatch DebugTrapPatch(uint32_t targetOffset_,
                                       DebugTrapHandlerKind dbgKind_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::DebugTrapHandler;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(dbgKind_);
      return p;
    }

    static RuntimePatch CppFunctionPatch(uint32_t targetOffset_,
                                         AOTCppFunctionId fnId) {
      RuntimePatch p;
      p.kind = AOTRelocKind::CppFunction;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(fnId);
      return p;
    }

    static RuntimePatch PreBarrierPatch(uint32_t targetOffset_,
                                        AOTPreBarrierIndex idx) {
      RuntimePatch p;
      p.kind = AOTRelocKind::PreBarrier;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(idx);
      return p;
    }

    static RuntimePatch ExceptionTailPatch(uint32_t targetOffset_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::ExceptionTail;
      p.targetOffset = targetOffset_;
      p.auxData = 0;
      return p;
    }

    explicit RuntimePatch(AOTRelocKind kind_, uint32_t targetOffset_) :
      kind(kind_), targetOffset(targetOffset_) {}

    void apply(const PatchContext& pc) const;

  private:
    RuntimePatch() = default;
    uintptr_t getValueToPatch(const PatchContext& pc) const;
};

static constexpr uintptr_t AOT_PATCH_SENTINEL = 0x0000A070DEADBEEF;

static_assert(sizeof(RuntimePatch) == 12,
              "RuntimePatch size must be 12 bytes");

using RuntimePatchVector = Vector<RuntimePatch, 0, SystemAllocPolicy>;

// =========================================================================
// Generic helpers
// =========================================================================

class JitCode;

void* ResolveCppFunction(AOTCppFunctionId id);

[[nodiscard]] JitCode* AllocateAndPatchAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    const uint8_t* containerBase, uint32_t headerSize,
    CodeKind codeKind, uint32_t dispatchTableOffset);

// =========================================================================
// AOT accumulator and compilation context
// =========================================================================

// Accumulates runtime patches during AOT codegen. Generic — usable by
// any AOT blob kind (baseline interp, baseline compilation, trampolines,
// IC stubs, etc.).
struct AOTAccumulator {
  RuntimePatchVector runtimePatches;

  void registerPatch(RuntimePatch&& patch) {
    MOZ_ALWAYS_TRUE(runtimePatches.append(std::move(patch)));
  }
};

// Unified AOT compilation context. Stack-allocated by the caller and passed
// to MacroAssembler as a non-owning pointer. When present (non-nullptr),
// indicates that AOT codegen mode is active.
class AOTContext {
 public:
  AOTContext() = default;

  void bindMasm(MacroAssembler& masm) { masm_ = &masm; }

  AOTAccumulator& accumulator() { return accumulator_; }

 private:
  MacroAssembler* masm_ = nullptr;
  AOTAccumulator accumulator_;
};

}  // namespace js::jit

#endif  // jit_AOT_h
