/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOT.h"

#include "mozilla/Maybe.h"

#include "gc/Zone.h"
#include "jit/JitCode.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "vm/JSContext.h"

namespace js::jit {

// ---------------------------------------------------------------------------
// JitCode allocation helper
// ---------------------------------------------------------------------------

JitCode* AllocateAOTCode(JSContext* cx,
                               const AOTBlobDirectoryEntry* entry,
                               uint8_t* textBase, CodeKind codeKind) {
  mozilla::Maybe<AutoAllocInAtomsZone> az;
  if (!cx->zone() || !cx->zone()->isAtomsZone()) {
    az.emplace(cx);
  }

  if (!cx->zone()->getJitZone(cx)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  uint8_t* codeStart = textBase + entry->codeOffset;
  JitCode* code = JitCode::NewStatic(cx, codeStart, entry->codeSize, codeKind);
  if (!code) {
    return nullptr;
  }

  return code;
}

mozilla::Maybe<AOTSlot> AOTIndirectionTable::findSlot(uintptr_t value) const {
  if (value == 0) return mozilla::Nothing();
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    if (slots_[i] == value) {
      return mozilla::Some(AOTSlot(i));
    }
  }
  return mozilla::Nothing();
}

void AOTIndirectionTable::dump() const {
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    JitSpew(JitSpew_BaselineAOT, "  slot[%u] %-30s = %p",
            i, AOTSlotName(AOTSlot(i)),
            (void*)get(AOTSlot(i)));
  }
}

AOTSlot AOTIndirectionTable::findSlotOrCrash(uintptr_t value) const {
  auto slot = findSlot(value);
  if (!slot) {
    JitSpew(JitSpew_BaselineAOT, "No AOT slot for %p", (void*)value);
    dump();
    MOZ_CRASH("No AOT slot for pointer");
  }
  return *slot;
}

}  // namespace js::jit
