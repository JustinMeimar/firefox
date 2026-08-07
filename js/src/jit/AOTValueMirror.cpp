/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOT.h"
#include "jit/CompileWrappers.h"

#include "jit/MacroAssembler-inl.h"

// [SMDOC] AOT Value Mirrors
//
// Most runtime addresses reach AOT code through an interception layer that
// rewrites the emitter's operands, so top level codegen never learns it is
// emitting relocatable code. That rewrite needs a stable address to key off.
//
// A few values are polled often enough that chasing the address dominates the
// check. For those the runtime republishes the value itself into an
// indirection table slot, trading a write on every update for one fewer
// dependent load on every poll. No address survives to key off, so these
// emitters must be named and called explicitly: a mirror name at a call site
// is the signal that the site opts out of the transparent rewrite.

using namespace js;
using namespace js::jit;

#ifdef ENABLE_JS_AOT
static Address AOTMirrorSlot(Register base, AOTSlot slot) {
  MOZ_ASSERT(AOTIndirectionTable::isMirrorSlot(slot));
  return Address(base, AOTIndirectionTable::offsetOfSlot(slot));
}
#endif

void MacroAssembler::branchMirroredInterruptBits(Condition cond, Imm32 rhs,
                                                 Register scratch,
                                                 Label* label) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    emitAOTLoadTableBase(scratch);
    branch32(cond, AOTMirrorSlot(scratch, AOTSlot::InterruptBitsValue), rhs,
             label);
    return;
  }
#endif
  branch32(cond, AbsoluteAddress(runtime()->addressOfInterruptBits()), rhs,
           label);
}

void MacroAssembler::branchMirroredJitStackLimit(Condition cond, Register rhs,
                                                 Register scratch,
                                                 Label* label) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    emitAOTLoadTableBase(scratch);
    branchPtr(cond, AOTMirrorSlot(scratch, AOTSlot::JitStackLimitValue), rhs,
              label);
    return;
  }
#endif
  branchPtr(cond, AbsoluteAddress(runtime()->addressOfJitStackLimit()), rhs,
            label);
}

void MacroAssembler::branchStackPtrRhsMirroredJitStackLimit(Condition cond,
                                                            Register scratch,
                                                            Label* label) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    emitAOTLoadTableBase(scratch);
    branchStackPtrRhs(cond, AOTMirrorSlot(scratch, AOTSlot::JitStackLimitValue),
                      label);
    return;
  }
#endif
  branchStackPtrRhs(cond, AbsoluteAddress(runtime()->addressOfJitStackLimit()),
                    label);
}

void MacroAssembler::branchMirroredNoMarkingBarrier(Label* label) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    // Cheap runtime wide test first; only while an incremental GC is active do
    // we pay for the precise per zone test, which has to load the zone from
    // the current execution context.
    emitAOTLoadTableBase(ScratchReg);
    branch32(Assembler::Equal,
             AOTMirrorSlot(ScratchReg, AOTSlot::PreBarrierZoneCount), Imm32(0),
             label);
    branchTestNeedsMarkingBarrierAnyZone(Assembler::Zero, label, ScratchReg);
    return;
  }
#endif
  branchTestNeedsMarkingBarrier(Assembler::Zero, label);
}
