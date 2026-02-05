/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"
#include <cstdint>
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "mozilla/Assertions.h"
#include "vm/JSContext.h"

namespace js::jit {

uintptr_t RuntimePatch::_getValueToPatch(const PatchContext& pc) const {
  switch(kind) {
    case Kind::WellKnownSymbols:
      return (uintptr_t)pc.cx->runtime()->wellKnownSymbols.ref();
    case Kind::JitRuntime:
      return (uintptr_t)pc.cx->runtime()->jitRuntime();
    case Kind::ContextRealm:
      return (uintptr_t)(reinterpret_cast<const uint8_t*>(pc.cx)
              + JSContext::offsetOfRealm());
    case Kind::JSContextPtr:
      return (uintptr_t)pc.cx;
    case Kind::DispatchTable:
      return (uintptr_t)(pc.codeBase + handlerOffset);
    case Kind::VMWrapper:
      TrampolinePtr ptr = pc.cx->runtime()->jitRuntime()->getVMWrapper(vmId);
      return (uintptr_t)(ptr.value);
  }
  MOZ_CRASH("Unexpected Patch Type");
}

void RuntimePatch::apply(const PatchContext& pc) const {
  uintptr_t val = _getValueToPatch(pc);
  uint8_t* target = pc.codeBase + targetOffset;
#ifdef DEBUG
  uintptr_t beforeValue = *reinterpret_cast<uintptr_t*>(target);
  JitSpew(JitSpew_BaselineAOT, "Runtime patch @ offset %u: before=0x%016lx after=0x%016lx",
          targetOffset, beforeValue, val);
#endif
  *reinterpret_cast<uintptr_t*>(target) = val;
}

}  // namespace js::jit
