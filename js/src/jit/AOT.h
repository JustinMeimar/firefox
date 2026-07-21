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

namespace js::jit {

extern const double MathRandomScaleInv;

// [SMDOC] AOT JIT Code
//
// When built with `ENABLE_JS_AOT`, SpiderMonkey emits relocatable JIT
// code for the baseline interpreter, inline cache stubs, and self-hosted
// builtins.
//
// To make emitted code position-independent w.r.t. runtime pointers,
// every value an AOT masm scope would otherwise bake in as an ImmPtr is
// enumerated in AOTSlot and resolved at run time by an indirect load
// through the JSRuntime-owned AOTIndirectionTable.
//
// Table access, per call path:
//   Baseline frame:      frame slot -> table base -> slot value
//                        (BaselineFrame::reverseOffsetOfAOTTableBase())
//   Baseline stub frame: frame slot -> table base -> slot value
//                        (BaselineStubFrameLayout::AOTTableOffsetFromFP)
// Both are two loads. Pinning the table base into a register (one load)
// is future work.

enum class AOTSlot : uint32_t {
#define AOT_SLOT(name, ...) name,
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
  Count
};

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
  AOTSlot findSlotOrCrash(uintptr_t value) const;
  void dump() const;

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTSlot::Count)] = {};
};

}  // namespace js::jit

#endif  // jit_AOT_h
