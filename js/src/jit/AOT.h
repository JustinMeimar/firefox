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
#include "mozilla/Maybe.h"

#include <cstdint>
#include <cstring>

#include "jstypes.h"

#include "jit/VMFunctions.h"
#include "js/Vector.h"
#include "vm/JSContext.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

// [SMDOC] AOT Slot Table
//
// AOTSlot enumerates every runtime address that AOT code needs.
// Each slot holds a single uintptr_t in the AOTIndirectionTable.

#define AOT_CORE_SLOTS(V)               \
  V(JSRuntimePtr)                       \
  V(JSContextPtr)                       \
  V(InterruptBits)                      \
  V(JitActivation)                      \
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
  V(CppFn_FrameIsDebuggeeCheck)         \
  V(CppFn_HandleCodeCoverageAtPrologue) \
  V(CppFn_HandleCodeCoverageAtPC)       \
  V(PreBarrier_Value)                   \
  V(PreBarrier_String)                  \
  V(PreBarrier_Object)                  \
  V(PreBarrier_Shape)                   \
  V(PreBarrier_WasmAnyRef)              \
  V(Class_WithEnvironmentObject)        \
  V(Class_PropertyIteratorObject)       \
  V(Class_Function)                     \
  V(Class_ExtendedFunction)             \
  V(DeadObjectProxySingleton)           \
  V(AtomEmpty)                          \
  V(AtomTrue)                           \
  V(AtomFalse)                          \
  V(StaticStringsUnitTable)             \
  V(StaticStringsLength2Table)          \
  V(StaticStringsIntTable)              \
  V(StaticStringsToSmallCharTable)

static constexpr uint32_t kAOTMaxVMWrappers = 512;
static constexpr uint32_t kAOTMaxABIFunctions = 256;

enum class AOTSlot : uint32_t {
#define EMIT_SLOT(name) name,
  AOT_CORE_SLOTS(EMIT_SLOT)
#undef EMIT_SLOT
  VMWrapper_Begin,
  VMWrapper_End = VMWrapper_Begin + kAOTMaxVMWrappers,
  ABIFn_Begin = VMWrapper_End,
  ABIFn_End = ABIFn_Begin + kAOTMaxABIFunctions,
  Count = ABIFn_End
};

inline AOTSlot AOTSlotForABIFn(uint32_t idx) {
  MOZ_ASSERT(idx < kAOTMaxABIFunctions);
  return AOTSlot(uint32_t(AOTSlot::ABIFn_Begin) + idx);
}

inline AOTSlot AOTSlotForVMWrapper(uint32_t id) {
  MOZ_ASSERT(id < kAOTMaxVMWrappers);
  return AOTSlot(uint32_t(AOTSlot::VMWrapper_Begin) + id);
}

inline const char* AOTSlotName(AOTSlot slot) {
  switch (slot) {
#define EMIT_CASE(name) case AOTSlot::name: return #name;
    AOT_CORE_SLOTS(EMIT_CASE)
#undef EMIT_CASE
    default:
      break;
  }
  if (uint32_t(slot) >= uint32_t(AOTSlot::VMWrapper_Begin) &&
      uint32_t(slot) < uint32_t(AOTSlot::VMWrapper_End)) {
    return "VMWrapper";
  }
  return "Unknown";
}



static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"
static constexpr uint32_t AOT_CONTAINER_VERSION = 3;

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  SelfHostedFunction = 1,
  InlineCacheStub = 2,
  /* Trampoline = 3 */
};

// [SMDOC] AOT Container Format
//
// The on-disk (and embedded) container is a flat binary with a fixed
// header, a directory of blob entries, and the blob payloads (code,
// manifest, metadata).  Each blob carries its own kind tag
// (interpreter vs. self-hosted function, ... ) so the loader can
// lookup the code and meta-data.
struct AOTContainerHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t blobCount;
  uint32_t padding;
};

static_assert(sizeof(AOTContainerHeader) == 16,
              "AOTContainerHeader must be 16 bytes");

// An `AOTBlobDirectoryEntry` is 1:1 with a masm buffer.
// The manifest describes the layout of the blob as
// composed of code and metadata.
struct AOTBlobDirectoryEntry {
  AOTBlobKind kind;
  uint32_t nameHash;
  uint32_t codeOffset;
  uint32_t codeSize;
  uint32_t metadataOffset;
  uint32_t metadataSize;
  uint32_t manifestOffset;
  uint32_t manifestSize;
};

static_assert(sizeof(AOTBlobDirectoryEntry) == 32,
              "AOTBlobDirectoryEntry must be 32 bytes");

extern "C" {
  extern const uint8_t bl_aot_container_start[];
  extern const uint8_t bl_aot_container_end[];
  extern uint8_t bl_aot_text_start[];
  extern uint8_t bl_aot_text_end[];
}

inline const uint8_t* GetAOTContainer() {
  return bl_aot_container_start;
}

inline size_t GetAOTContainerSize() {
  return bl_aot_container_end - bl_aot_container_start;
}

inline uint8_t* GetAOTTextBase() {
  return bl_aot_text_start;
}

inline size_t GetAOTTextSize() {
  return bl_aot_text_end - bl_aot_text_start;
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

template <typename Fn>
inline bool ForEachAOTBlob(AOTBlobKind kind, Fn&& fn) {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return false;
  const auto* dir = GetAOTBlobDirectory();
  bool found = false;
  for (uint32_t i = 0; i < hdr->blobCount; i++) {
    if (dir[i].kind == kind && dir[i].codeSize > 0) {
      fn(&dir[i]);
      found = true;
    }
  }
  return found;
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


class JitCode;

[[nodiscard]] JitCode* AllocateAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    const uint8_t* containerBase, uint32_t headerSize,
    CodeKind codeKind);

// Allocate a JitCode that points directly at static .text code.
// No memcpy, no JIT pool allocation.
[[nodiscard]] JitCode* AllocateStaticAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    uint8_t* textBase, CodeKind codeKind);

// A table of runtime pointers that AOT baseline code loads from
// to attain position indpendence. Owned inline by JitRuntime.
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

  mozilla::Maybe<AOTSlot> findSlot(uintptr_t value) const;
  AOTSlot findSlotOrCrash(uintptr_t value) const;
  void dump() const;

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTSlot::Count)];
};

// [SMDOC] AOT Compilation Context
//
// AOTContext is the single flag that switches the codegen pipeline into
// AOT mode.  Stack-allocated by the caller and passed to MacroAssembler
// as a non-owning pointer.  When present (non-nullptr), AOT codegen is
// active: runtime pointers are loaded via a TLS-based pointer chain
// instead of baked-in absolute addresses.
class AOTContext {
 public:
  explicit AOTContext(AOTIndirectionTable* table) : table_(table) {}
  void bindMasm(MacroAssembler& masm) { masm_ = &masm; }
  AOTIndirectionTable* indirectionTable() const { return table_; }
 private:
  MacroAssembler* masm_ = nullptr;
  [[maybe_unused]] AOTIndirectionTable* table_;
};

}  // namespace js::jit

#endif  // jit_AOT_h
