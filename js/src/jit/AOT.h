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

namespace JS {
class Zone;
}

namespace js::jit {

class JitCode;

extern const double MathRandomScaleInv;

// [SMDOC] AOT JIT Code
//
// When built with `ENABLE_JS_AOT`, SpiderMonkey emits relocatable JIT
// code for the baseline interpreter, inline cache stubs, and self-hosted
// builtins.
//
// Runtime-wide pointers are enumerated in AOTSlot and resolved through
// the JSRuntime-owned AOTIndirectionTable. Zone-relative operands declared
// in AOTSlots.tbl are rebuilt from the current JSContext's Zone.
//
// Table access, per call path:
//   Baseline frame:      frame slot -> table base -> slot value
//                        (BaselineFrame::reverseOffsetOfAOTTableBase())
//   Baseline stub frame: frame slot -> table base -> slot value
//                        (BaselineStubFrameLayout::AOTTableOffsetFromFP)
// Both are two loads. Pinning the table base into a register (one load)
// is future work.

// Extended-slot capacities. Each region is a fixed range in the AOTSlot
// enum, populated at runtime from JitRuntime state; codegen looks them
// up by index via the helpers below. Sizes are conservative caps and
// verified by MOZ_ASSERT in populateAOTIndirectionTable.
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

mozilla::Maybe<int32_t> FindAOTZoneAddressOffset(uintptr_t address,
                                                 JS::Zone* zone);
mozilla::Maybe<int32_t> FindAOTZoneValueOffset(uintptr_t value,
                                               JS::Zone* zone);

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

// Ensure a preamble trampoline exists for |code|. The trampoline seeds
// |passReg| with the runtime's AOT indirection table base and jumps to
// code->raw(). Callers must pass the register the emitted code reads
// from: AOTFuncPassReg for baseline JIT functions, AOTInterpPassReg for
// the baseline interpreter. Idempotent.
[[nodiscard]] bool EnsureAOTPreambleTrampolineFor(JSContext* cx, JitCode* code,
                                                  Register passReg);

}  // namespace js::jit

#endif  // jit_AOT_h
