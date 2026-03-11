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

#include "mozilla/HashFunctions.h"

#include <cstdint>
#include <cstring>

#include "jstypes.h"

#include "jit/VMFunctions.h"
#include "js/Vector.h"
#include "vm/JSContext.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class MacroAssembler;

// [SMDOC] AOT Relocation Kinds
//
// The AOT blob cannot embed absolute pointers because ASLR, runtime
// layout, and even the set of compiled C++ helpers change between
// processes.  AOTRelocKind enumerates every category of address that
// must be fixed up at load time.
//
// During normal (non-AOT) codegen each kind resolves to an ImmPtr
// immediately.  In AOT mode the codegen emits a movWithPatch with a
// sentinel immediate and registers a RuntimePatch; the loader walks
// the patch table and writes the real addresses before the code runs.

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
  V(AOTTableBase)

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

// [SMDOC] AOT Container Format
//
// The on-disk (and embedded) container is a flat binary with a fixed
// header, a directory of blob entries, and the blob payloads (code,
// manifest, patches, metadata).  Each blob carries its own kind tag
// (interpreter vs. self-hosted function) so the loader can find the
// right entry without external metadata.

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
  AOTBlobKind kind;
  uint32_t nameHash;
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
    if (dir[i].kind == kind &&
        (nameHash == 0 || dir[i].nameHash == nameHash) &&
        dir[i].codeSize > 0) {
      return &dir[i];
    }
  }
  return nullptr;
}


// [SMDOC] AOT Runtime Patching
//
// At load time every RuntimePatch is applied in order: the loader
// resolves each patch's AOTRelocKind to a concrete address via
// PatchContext, then writes that address into the code blob at the
// recorded targetOffset.  RuntimePatch is a 12-byte POD so the patch
// table can be memcpy'd straight out of the container.

// Load time context required to apply patches.
struct PatchContext {
    JSContext* cx;
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;
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

class JitCode;

[[nodiscard]] JitCode* AllocateAndPatchAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    const uint8_t* containerBase, uint32_t headerSize,
    CodeKind codeKind, uint32_t dispatchTableOffset);

// [SMDOC] AOT Accumulator and Compilation Context
//
// AOTAccumulator collects RuntimePatch entries as codegen proceeds.
// AOTContext bundles the accumulator with a back-pointer to the
// MacroAssembler; it is stack-allocated by the caller and passed as a
// non-owning pointer.  A non-null AOTContext on the MacroAssembler is
// the single flag that switches the whole pipeline into AOT mode.
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


// Slots available in the AOTIndirectionTable.  Each slot holds a single
// uintptr_t that AOT code loads at runtime instead of relying on a patched
// movabs.  One slot per scalar (non-parametric) AOTRelocKind.
enum class AOTIndirectionSlot : uint32_t {
  JSRuntimePtr = 0,
  JSContextPtr,
  InterruptBits,
  JitActivation,
  RealmPtr,
  ContextRealm,
  WellKnownSymbols,
  JitRuntime,
  LastBufferedCell,
  ProfilerEnabled,
  ProfilerExitFrameTail,
  DoubleToInt32Stub,
  MegamorphicCache,
  MegamorphicSetPropCache,
  StringToAtomCache,
  ExceptionTail,
  DebugTrapInterpreter,
  DebugTrapCompiler,
  CppFn_PostWriteBarrier,
  CppFn_FrameIsDebuggeeCheck,
  CppFn_HandleCodeCoverageAtPrologue,
  CppFn_HandleCodeCoverageAtPC,
  PreBarrier_Value,
  PreBarrier_String,
  PreBarrier_Object,
  PreBarrier_Shape,
  PreBarrier_WasmAnyRef,
  VMWrapperBase,
  Count
};

// A small, flat table of runtime pointers that AOT baseline code loads
// indirectly via a patched movabs of the table base (AOTRelocKind::AOTTableBase)
// followed by a slot load.  Owned inline by JitRuntime so its address is stable.
class AOTIndirectionTable {
 public:
  AOTIndirectionTable() { std::memset(slots_, 0, sizeof(slots_)); }

  void set(AOTIndirectionSlot slot, uintptr_t value) {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTIndirectionSlot::Count));
    slots_[uint32_t(slot)] = value;
  }

  uintptr_t get(AOTIndirectionSlot slot) const {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTIndirectionSlot::Count));
    return slots_[uint32_t(slot)];
  }

  static constexpr uint32_t offsetOfSlot(AOTIndirectionSlot slot) {
    return uint32_t(slot) * sizeof(uintptr_t);
  }

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTIndirectionSlot::Count)];
};

}  // namespace js::jit

#endif  // jit_AOT_h
