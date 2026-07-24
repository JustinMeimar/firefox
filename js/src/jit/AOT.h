/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <cstdint>

#include "jstypes.h"

#include "jit/Registers.h"

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

// Reserves fixed index ranges for groups of runtime supplied entries. The
// limits are intentionally conservative and checked when the table is
// populated.
static constexpr uint32_t AOTMaxVMWrappers = 512;
static constexpr uint32_t AOTMaxABIFunctions = 256;

enum class AOTSlot : uint32_t {
#define AOT_SLOT(name, ...) name,
#define AOT_ATOM_SLOT AOT_SLOT
#include "jit/AOTSlots.tbl"
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
  NamedSlot_End,
  VMWrapper_Begin = NamedSlot_End,
  VMWrapper_End = VMWrapper_Begin + AOTMaxVMWrappers,
  ABIFn_Begin = VMWrapper_End,
  ABIFn_End = ABIFn_Begin + AOTMaxABIFunctions,
  Count = ABIFn_End
};

inline AOTSlot AOTSlotForVMWrapper(uint32_t id) {
  MOZ_ASSERT(id < AOTMaxVMWrappers);
  return AOTSlot(uint32_t(AOTSlot::VMWrapper_Begin) + id);
}

inline AOTSlot AOTSlotForABIFn(uint32_t idx) {
  MOZ_ASSERT(idx < AOTMaxABIFunctions);
  return AOTSlot(uint32_t(AOTSlot::ABIFn_Begin) + idx);
}

const char* AOTSlotName(AOTSlot slot);

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
