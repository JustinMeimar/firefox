/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h

#include "mozilla/Assertions.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"

#include <cstdint>
#include <iterator>

#include "jstypes.h"

#include "jit/ABIFunctionList.h"
#include "jit/Registers.h"
#include "js/experimental/TypedData.h"  // JS_FOR_EACH_TYPED_ARRAY

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class JitCode;

extern const double MathRandomScaleInv;

// [SMDOC] AOT JIT Code
//
// SpiderMonkey can emit relocatable code for the baseline interpreter, inline
// cache stubs, and self hosted functions. Runtime addresses are loaded through
// an indirection table so generated code does not embed process specific
// pointers. Baseline and stub frames each store the table address used by
// generated code.

// The wrapper range is a fixed reservation because a trampoline is only
// generated for a wrapper the first time something calls it, so the runtime
// count is not known at compile time.
static constexpr uint32_t AOTMaxVMWrappers = 512;

// Only the shape of the ABI function list matters here, so the entries are
// never named or evaluated; that keeps the declarations behind them out of
// this header. The array size fixes the ABIFn slot range and is the same
// count the image shim's link table binds against.
inline constexpr bool kAOTABIFnLinkable[] = {
#define AOT_ABIFN(fp) true,
#define AOT_ABIFN_NOLINK(fp) false,
#define AOT_ABIFN_TYPED(fp, ...) true,
#include "jit/AOTABIFns.tbl"
#undef AOT_ABIFN_TYPED
#undef AOT_ABIFN_NOLINK
#undef AOT_ABIFN
};

inline constexpr uint32_t kAOTABIFnCount = std::size(kAOTABIFnLinkable);

enum class AOTSlot : uint32_t {
  // Mirrored values rather than addresses. The runtime rewrites these
  // whenever the source of truth changes, saving generated code a
  // dereference on hot checks. They lead the table so that their offsets
  // reach an eight bit displacement, and the most frequently emitted one
  // needs no displacement at all.
  PreBarrierZoneCount = 0,
  InterruptBitsValue,
  JitStackLimitValue,

#define AOT_SLOT(name, ...) name,
#define AOT_ATOM_SLOT AOT_SLOT
#define AOT_LINK_SLOT AOT_SLOT
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
  NamedSlot_End,
  VMWrapper_Begin = NamedSlot_End,
  VMWrapper_End = VMWrapper_Begin + AOTMaxVMWrappers,
  ABIFn_Begin = VMWrapper_End,
  ABIFn_End = ABIFn_Begin + kAOTABIFnCount,
  Count = ABIFn_End,

  // Region markers, declared last so they alias entries already numbered
  // rather than consuming indices of their own.
  Mirror_Begin = PreBarrierZoneCount,
  Mirror_End = JitStackLimitValue + 1,
  NamedSlot_Begin = Mirror_End,
};

inline AOTSlot AOTSlotForVMWrapper(uint32_t id) {
  MOZ_ASSERT(id < AOTMaxVMWrappers);
  return AOTSlot(uint32_t(AOTSlot::VMWrapper_Begin) + id);
}

constexpr AOTSlot AOTSlotForABIFn(uint32_t idx) {
  MOZ_ASSERT(idx < kAOTABIFnCount);
  return AOTSlot(uint32_t(AOTSlot::ABIFn_Begin) + idx);
}

constexpr bool IsNamedAOTLinkSlot(AOTSlot slot) {
  switch (slot) {
#define AOT_SLOT(name, ...)
#define AOT_ATOM_SLOT(name, ...)
#define AOT_LINK_SLOT(name, ...) case AOTSlot::name:
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
    return true;
    default:
      return false;
  }
}

// True when the slot's value is fixed at link time, so generated code can
// reference it with a relocation the static linker resolves rather than an
// indirection table load. The image shim must carry a specialization for every
// such slot. Out of line because the ABI function half of the answer comes
// from a table too heavy to pull into every macro assembler consumer.
bool IsAOTLinkSlot(AOTSlot slot);

// Identifies the slot numbering this build compiles against. Link sites name
// slots by index, so an image recorded against a different numbering would
// bind to the wrong symbol. Hashing the effective slot list rather than the
// table source also covers conditionally compiled entries, which shift every
// following index. The region boundaries go in too, because moving a region
// renumbers everything after it without changing any name.
constexpr mozilla::HashNumber AOTSlotTableHash() {
  const char* const names[] = {
#define AOT_SLOT(name, ...) #name,
#define AOT_ATOM_SLOT(name, ...) "@" #name,
#define AOT_LINK_SLOT(name, ...) "&" #name,
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
  };
  mozilla::HashNumber h = 0;
  for (const char* n : names) {
    h = mozilla::AddToHash(h, mozilla::HashStringUntilZero(n));
  }
  const uint32_t layout[] = {uint32_t(AOTSlot::Mirror_Begin),
                             uint32_t(AOTSlot::Mirror_End),
                             uint32_t(AOTSlot::NamedSlot_Begin),
                             uint32_t(AOTSlot::NamedSlot_End),
                             uint32_t(AOTSlot::VMWrapper_Begin),
                             uint32_t(AOTSlot::VMWrapper_End),
                             uint32_t(AOTSlot::ABIFn_Begin),
                             uint32_t(AOTSlot::ABIFn_End),
                             uint32_t(AOTSlot::Count)};
  for (uint32_t v : layout) {
    h = mozilla::AddToHash(h, v);
  }
  return h;
}

const char* AOTSlotName(AOTSlot slot);

// A four byte rip relative displacement inside recorded code that the next
// build's static linker fills in from the slot's symbol. The displacement is
// left zero at capture time; the recorded bytes are never executed.
struct AOTLinkSite {
  uint32_t codeOffset;
  uint32_t slot;
};

static_assert(sizeof(AOTLinkSite) == 8, "AOTLinkSite is written to .aotb");

class AOTIndirectionTable {
 public:
  AOTIndirectionTable() = default;

  void set(AOTSlot slot, uintptr_t value) {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    slots_[uint32_t(slot)] = value;
  }

  uintptr_t get(AOTSlot slot) const {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    return slots_[uint32_t(slot)];
  }

  static constexpr bool isMirrorSlot(AOTSlot slot) {
    static_assert(uint32_t(AOTSlot::Mirror_Begin) == 0);
    return uint32_t(slot) < uint32_t(AOTSlot::Mirror_End);
  }

  // Mirror slots may be rewritten from another thread while jit code on the
  // owning thread polls them.
  void setMirrored(AOTSlot slot, uintptr_t value) {
    MOZ_ASSERT(isMirrorSlot(slot));
    __atomic_store_n(&slots_[uint32_t(slot)], value, __ATOMIC_SEQ_CST);
  }

  static constexpr uint32_t offsetOfSlot(AOTSlot slot) {
    return uint32_t(slot) * sizeof(uintptr_t);
  }

  mozilla::Maybe<AOTSlot> findSlot(uintptr_t value) const;
  mozilla::Maybe<AOTSlot> findAtomSlot(uintptr_t value) const;
  AOTSlot findSlotOrCrash(uintptr_t value) const;
  void dump() const;

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTSlot::Count)] = {};
};

// Creates a preamble for static code when needed. The preamble initializes the
// runtime indirection table and enters the target using the register expected
// by that entry path. Repeated requests return the existing preamble.
[[nodiscard]] bool EnsureAOTPreambleTrampolineFor(JSContext* cx, JitCode* code,
                                                  Register passReg);

}  // namespace js::jit

#endif  // jit_AOT_h
