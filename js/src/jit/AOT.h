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

// [SMDOC] AOT Slot Table
//
// AOTSlot enumerates every runtime address that AOT code needs.
// Each slot holds a single uintptr_t in the AOTIndirectionTable.
//
// Non-AOT codegen: the table is populated at JitRuntime init time;
// moveAOTSlot reads directly from it.
//
// AOT codegen: moveAOTSlot emits a patched movabs of the table base
// followed by a slot load.  The only RuntimePatch kind is the table
// base address.

#define AOT_SLOTS(V)                    \
  V(JSRuntimePtr)                       \
  V(JSContextPtr)                       \
  V(InterruptBits)                      \
  V(JitActivation)                      \
  V(RealmPtr)                           \
  V(ContextRealm)                       \
  V(WellKnownSymbols)                   \
  V(JitRuntime)                         \
  V(LastBufferedCell)                    \
  V(ProfilerEnabled)                    \
  V(ProfilerExitFrameTail)              \
  V(DoubleToInt32Stub)                  \
  V(MegamorphicCache)                   \
  V(MegamorphicSetPropCache)            \
  V(StringToAtomCache)                  \
  V(ExceptionTail)                      \
  V(DebugTrapInterpreter)               \
  V(DebugTrapCompiler)                  \
  V(CppFn_PostWriteBarrier)             \
  V(CppFn_FrameIsDebuggeeCheck)        \
  V(CppFn_HandleCodeCoverageAtPrologue) \
  V(CppFn_HandleCodeCoverageAtPC)       \
  V(PreBarrier_Value)                   \
  V(PreBarrier_String)                  \
  V(PreBarrier_Object)                  \
  V(PreBarrier_Shape)                   \
  V(PreBarrier_WasmAnyRef)              \
  V(VMWrapperBase)

enum class AOTSlot : uint32_t {
#define EMIT_SLOT(name) name,
  AOT_SLOTS(EMIT_SLOT)
#undef EMIT_SLOT
  Count
};

inline const char* AOTSlotName(AOTSlot slot) {
  switch (slot) {
#define EMIT_CASE(name) case AOTSlot::name: return #name;
    AOT_SLOTS(EMIT_CASE)
#undef EMIT_CASE
    case AOTSlot::Count:
      break;
  }
  return "Unknown";
}

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
  //TODO(Justin): Add support for additional AOT types.
  /* InlineCacheStub = 2, */
  /* Trampoline = 3 */
};

struct AOTContainerHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t blobCount;
  uint32_t padding;
};

static_assert(sizeof(AOTContainerHeader) == 16,
              "AOTContainerHeader must be 16 bytes");

// An `AOTBlobDirectoryEntry` is 1:1 with a masm buffer.
// The manfiest describes the layout of the blob as
// composed of code, metadata and patches.
struct AOTBlobDirectoryEntry {
  AOTBlobKind kind;
  uint32_t nameHash;
  uint32_t codeOffset;
  uint32_t codeSize; 
  uint32_t patchesOffset;
  uint32_t patchesCount;
  uint32_t metadataOffset;
  uint32_t metadataSize; 
  uint32_t manifestOffset;
  uint32_t manifestSize;
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
// The only value that still needs a patched movabs is the table base
// address itself (every other value lives in the table).  RuntimePatch
// records where each movabs sentinel lives so the loader can write
// the real table base pointer.

// Load time context required to apply patches.
struct PatchContext {
    JSContext* cx;
    uint8_t* codeBase;
};

class RuntimePatch {
  public:
    // targetOffset: byte offset within the code blob where the sentinel
    // immediate lives.  The patched value is always the AOT table base.
    uint32_t targetOffset;
    explicit RuntimePatch(uint32_t targetOffset_) :
      targetOffset(targetOffset_) {}

    void apply(const PatchContext& pc) const;
};

static constexpr uintptr_t AOT_PATCH_SENTINEL = 0x0000A070DEADBEEF;

static_assert(sizeof(RuntimePatch) == 4,
              "RuntimePatch size must be 4 bytes");

using RuntimePatchVector = Vector<RuntimePatch, 0, SystemAllocPolicy>;

class JitCode;

[[nodiscard]] JitCode* AllocateAndPatchAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    const uint8_t* containerBase, uint32_t headerSize,
    CodeKind codeKind);

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


// A small, flat table of runtime pointers that AOT baseline code loads
// indirectly via a patched movabs of the table base followed by a slot
// load.  Owned inline by JitRuntime so its address is stable.
// Indexed by AOTSlot.
class AOTIndirectionTable {
 public:
  AOTIndirectionTable() { std::memset(slots_, 0, sizeof(slots_)); }

  void set(AOTSlot slot, uintptr_t value) {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    slots_[uint32_t(slot)] = value;
  }

  uintptr_t get(AOTSlot slot) const {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    return slots_[uint32_t(slot)];
  }

  static constexpr uint32_t offsetOfSlot(AOTSlot slot) {
    return uint32_t(slot) * sizeof(uintptr_t);
  }

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTSlot::Count)];
};

}  // namespace js::jit

#endif  // jit_AOT_h
