/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h


#include "mozilla/Maybe.h"
#include <cstdint>
#include <cstring>
#include "jstypes.h"
#include "vm/JSContext.h"

namespace js::jit {

// [SMDOC] AOT JIT Code 

// When compiled with ENABLE_AOT_BASELINE, SpiderMonkey can emit
// relocatable JIT code for the BaselineInterpreter, Inline
// Cache Stubs and self-hosted Baseline compiled builtins.
//
// To make JIT code relocatable, all uses of `ImmPtr` are intercepted inside
// a set of common masm interfaces then compared against a set
// of expected pointers enumerated in `AOTSlot`. If the pointer
// can be identified, the masm emits an indirection to attain the
// pointer via the &AOTIndirectionTable, stored in the BaselineFrame.
// This incurs a cost of two loads to attain any runtime pointer, but
// allows the generated code to be independent from any particular runtime.
// The buffer can then be dumped, serialized into an assembly scaffold,
// and reattached as a build input.

extern const double MathRandomScaleInv;

static constexpr uint32_t kAOTMaxVMWrappers = 512;
static constexpr uint32_t kAOTMaxABIFunctions = 256;

enum class AOTSlot : uint32_t {
#define AOT_SLOT(name, ...) name,
#define AOT_SLOT_TRAMPOLINE(name) name,
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
#undef AOT_SLOT_TRAMPOLINE
  NamedSlot_End,
  VMWrapper_Begin = NamedSlot_End,
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
#define AOT_SLOT(name, ...) case AOTSlot::name: return #name;
#define AOT_SLOT_TRAMPOLINE(name) case AOTSlot::name: return #name;
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
#undef AOT_SLOT_TRAMPOLINE
    default:
      break;
  }
  uint32_t s = uint32_t(slot);
  if (s >= uint32_t(AOTSlot::VMWrapper_Begin) &&
      s < uint32_t(AOTSlot::VMWrapper_End)) {
    return "VMWrapper";
  }
  return "Unknown";
}

static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  SelfHostedFunction = 1,
  InlineCacheStub = 2
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

inline const uint8_t* GetAOTContainer() { return bl_aot_container_start; }
inline uint8_t* GetAOTTextBase() { return bl_aot_text_start; }
inline size_t GetAOTTextSize() { return bl_aot_text_end - bl_aot_text_start; }
inline size_t GetAOTContainerSize() { return bl_aot_container_end - bl_aot_container_start; }

inline const AOTContainerHeader* GetAOTContainerHeader() {
  if (GetAOTContainerSize() < sizeof(AOTContainerHeader)) {
    return nullptr;
  }
  const auto* hdr = reinterpret_cast<const AOTContainerHeader*>(GetAOTContainer());
  if (hdr->magic != AOT_CONTAINER_MAGIC) {
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

// Allocate a JitCode that points directly at static .text AOT code.
[[nodiscard]] JitCode* AllocateAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    uint8_t* textBase, CodeKind codeKind);

// A table of runtime pointers that AOT baseline code loads from
// to attain position independence. Owned inline by JitRuntime.
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
// active: runtime pointers are loaded via AOTIndirectionTable. 
class AOTContext {
 public:
  explicit AOTContext(AOTIndirectionTable* table) : table_(table) {}
  AOTIndirectionTable* indirectionTable() const { return table_; }
 private:
  AOTIndirectionTable* table_;
};

}  // namespace js::jit

#endif  // jit_AOT_h
