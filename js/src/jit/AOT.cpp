/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOT.h"

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include "jit/JitSpewer.h"

namespace js::jit {

const char* AOTSlotName(AOTSlot slot) {
  switch (slot) {
#define AOT_SLOT(name, ...) \
  case AOTSlot::name:       \
    return #name;
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
    default:
      break;
  }
  uint32_t s = uint32_t(slot);
  if (s >= uint32_t(AOTSlot::VMWrapper_Begin) &&
      s < uint32_t(AOTSlot::VMWrapper_End)) {
    return "VMWrapper";
  }
  if (s >= uint32_t(AOTSlot::ABIFn_Begin) &&
      s < uint32_t(AOTSlot::ABIFn_End)) {
    return "ABIFn";
  }
  return "Unknown";
}

mozilla::Maybe<AOTSlot> AOTIndirectionTable::findSlot(uintptr_t value) const {
  if (value == 0) {
    return mozilla::Nothing();
  }
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    if (slots_[i] == value) {
      return mozilla::Some(AOTSlot(i));
    }
  }
  return mozilla::Nothing();
}

void AOTIndirectionTable::dump() const {
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    JitSpew(JitSpew_BaselineAOT, "  slot[%u] %-30s = %p", i,
            AOTSlotName(AOTSlot(i)),
            reinterpret_cast<void*>(get(AOTSlot(i))));
  }
}

AOTSlot AOTIndirectionTable::findSlotOrCrash(uintptr_t value) const {
  auto slot = findSlot(value);
  if (!slot) {
    JitSpew(JitSpew_BaselineAOT, "No AOT slot for %p",
            reinterpret_cast<void*>(value));
    dump();
    MOZ_CRASH("No AOT slot for pointer");
  }
  return *slot;
}

}  // namespace js::jit
