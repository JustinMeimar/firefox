/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AOT-specific MacroAssembler methods.
//
// All methods here are part of MacroAssembler but are separated into their
// own translation unit to keep AOT codegen complexity isolated.  Every method
// is dual-mode: it works in both AOT and non-AOT compilation, branching
// internally on isAOT().

#include "jit/BaselineFrame.h"
#include "jit/JitRuntime.h"
#include "jit/VMFunctions.h"
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::jit;

#ifdef ENABLE_AOT_BASELINE

void MacroAssembler::emitAOTSlotLoad(AOTSlot slot, Register dest) {
  if (inAOTStubFrame_) {
    // In IC code the frame-pointer is set to the ICFrame, rather than
    // the baseline frame. We pay an extra load to reach the BaselineFrame.
    MacroAssemblerSpecific::loadPtr(Address(FramePointer, 0), dest);
    MacroAssemblerSpecific::loadPtr(
        Address(dest, BaselineFrame::reverseOffsetOfAOTTableBase()), dest);
  } else {
    MacroAssemblerSpecific::loadPtr(
        Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()), dest);
  }
  int32_t slotOff = int32_t(AOTIndirectionTable::offsetOfSlot(slot));
  MacroAssemblerSpecific::loadPtr(Address(dest, slotOff), dest);
}

static AOTSlot PreBarrierSlotForMIRType(MIRType type) {
  switch (type) {
    case MIRType::Value:
      return AOTSlot::PreBarrier_Value;
    case MIRType::String:
      return AOTSlot::PreBarrier_String;
    case MIRType::Object:
      return AOTSlot::PreBarrier_Object;
    case MIRType::Shape:
      return AOTSlot::PreBarrier_Shape;
    case MIRType::WasmAnyRef:
      return AOTSlot::PreBarrier_WasmAnyRef;
    default:
      MOZ_CRASH("Unexpected MIRType for pre-barrier");
  }
}

void MacroAssembler::callPreBarrierAOT(MIRType type, Register scratch) {
  MOZ_ASSERT(isAOT());
  MOZ_ASSERT(scratch != PreBarrierReg);
  emitAOTSlotLoad(PreBarrierSlotForMIRType(type), scratch);
  call(scratch);
}

void MacroAssembler::loadZoneForAOT(Register dest) {
  MOZ_ASSERT(isAOT());
  loadJSContext(dest);
  MacroAssemblerSpecific::loadPtr(Address(dest, JSContext::offsetOfZone()),
                                  dest);
}

#endif  // ENABLE_AOT_BASELINE

void MacroAssembler::loadRuntime(Register reg) {
  movePtr(ImmPtr(runtime()), reg);
}

void MacroAssembler::movePtr(ImmPtr imm, Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    auto slot = aot().indirectionTable()->findSlot(val);
    if (slot) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
    // Values that are safe to bake into AOT code:
    //  - nullptr (always zero)
    //  - values fitting in 32 bits (encoded as imm32, position-independent)
    if (val != 0 && val > UINT32_MAX) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "AOT: no indirection slot for ImmPtr %p -- add this address to "
          "the AOT indirection table.",
          imm.value);
    }
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

void MacroAssembler::movePtr(ImmGCPtr imm, Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (MOZ_UNLIKELY(isAOT())) {
    auto slot = aot().indirectionTable()->findSlot(uintptr_t(imm.value));
    if (slot) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
    // GC pointers not in the table (e.g., script-specific atoms in the
    // interpreter codegen) fall through to emit a normal relocatable move.
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

void MacroAssembler::storePtr(ImmPtr imm, const Address& address) {
#ifdef ENABLE_AOT_BASELINE
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    auto slot = aot().indirectionTable()->findSlot(val);
    if (slot) {
      ScratchRegisterScope scratch(*this);
      emitAOTSlotLoad(*slot, scratch);
      MacroAssemblerSpecific::storePtr(scratch, address);
      return;
    }
    if (val != 0 && val > UINT32_MAX) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "AOT: no indirection slot for ImmPtr %p in storePtr -- add this "
          "address to the AOT indirection table.",
          imm.value);
    }
  }
#endif
  MacroAssemblerSpecific::storePtr(imm, address);
}

// Rather than clutter BaselineCodeGen with a bifurcated ifdef, we abstract,
// even though there is only one use, the logic for filling the blinterp
// dispatch table.
// In AOT mode, dispatch table entries are PIC-friendly int32 relative offsets.
// In non-AOT mode, they are absolute code pointers patched via CodeLabel.
void MacroAssembler::writeDispatchTableEntry(uint32_t tableOffset,
                                              size_t index,
                                              const Label& handler) {
  MOZ_ASSERT(handler.bound());
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    int32_t relOffset = int32_t(handler.offset()) - int32_t(tableOffset);
    writeInt32Data(relOffset);
    return;
  }
#endif
  CodeLabel cl;
  writeCodePointer(&cl);
  cl.target()->bind(handler.offset());
  addCodeLabel(cl);
}
