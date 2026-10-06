/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

#ifdef ENABLE_JS_AOT

void MacroAssembler::emitAOTLoadTableBase(Register dest) {
  MOZ_RELEASE_ASSERT(aotTableAddress_);
  MacroAssemblerSpecific::loadPtr(aotTableAddress_->address, dest);
  if (aotTableAddress_->throughCaller) {
    MacroAssemblerSpecific::loadPtr(
        Address(dest, BaselineFrame::reverseOffsetOfAOTTableBase()), dest);
  }
}

void MacroAssembler::emitAOTSlotLoad(AOTSlot slot, Register dest) {
  emitAOTLoadTableBase(dest);
  int32_t slotOff = int32_t(AOTIndirectionTable::offsetOfSlot(slot));
  MacroAssemblerSpecific::loadPtr(Address(dest, slotOff), dest);
}

void MacroAssembler::emitAOTAddress(AOTSlot slot, Register dest) {
  if (IsAOTLinkSlot(slot)) {
    emitAOTLinkAddress(slot, dest);
  } else {
    emitAOTSlotLoad(slot, dest);
  }
}

void MacroAssembler::emitAOTSlotCall(AOTSlot slot, Register scratch) {
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  emitAOTLoadTableBase(scratch);
  call(Address(scratch, int32_t(AOTIndirectionTable::offsetOfSlot(slot))));
#  else
  emitAOTSlotLoad(slot, scratch);
  call(scratch);
#  endif
}

void MacroAssembler::emitAOTSlotJump(AOTSlot slot, Register scratch) {
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  emitAOTLoadTableBase(scratch);
  MacroAssemblerSpecific::jump(
      Address(scratch, int32_t(AOTIndirectionTable::offsetOfSlot(slot))));
#  else
  emitAOTSlotLoad(slot, scratch);
  MacroAssemblerSpecific::jump(scratch);
#  endif
}

#  ifdef JS_CODEGEN_X64
// The encoders return the offset just past the instruction, so its
// displacement occupies the four bytes before that.
static uint32_t DisplacementOffset(CodeOffset afterInstruction) {
  MOZ_ASSERT(afterInstruction.offset() >= sizeof(int32_t));
  return uint32_t(afterInstruction.offset()) - sizeof(int32_t);
}

void MacroAssembler::emitAOTLinkAddress(AOTSlot slot, Register dest) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::leaRipRelative(dest);
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot)}));
}

void MacroAssembler::emitAOTLinkCall(AOTSlot slot) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::callWithPatch();
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot)}));
}

void MacroAssembler::emitAOTLinkLoad(AOTSlot slot, Register dest) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::loadRipRelativeInt64(dest);
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot)}));
}

#  else
void MacroAssembler::emitAOTLinkAddress(AOTSlot slot, Register dest) {
  MOZ_CRASH("AOT link slots are not implemented for this backend");
}

void MacroAssembler::emitAOTLinkCall(AOTSlot slot) {
  MOZ_CRASH("AOT link slots are not implemented for this backend");
}

void MacroAssembler::emitAOTLinkLoad(AOTSlot slot, Register dest) {
  MOZ_CRASH("AOT link slots are not implemented for this backend");
}
#  endif

void MacroAssembler::emitAOTDispatch(Register opcodeReg, Register tableReg) {
  if (isAOT()) {
    // AOT dispatch tables store int32 offsets relative to the table base.
    BaseIndex entry(tableReg, opcodeReg, TimesFour);
    load32SignExtendToPtr(entry, opcodeReg);
    addPtr(tableReg, opcodeReg);
    jump(opcodeReg);
  } else {
    BaseIndex pointer(tableReg, opcodeReg, ScalePointer);
    branchToComputedAddress(pointer);
  }
}

size_t MacroAssembler::aotDispatchTableEntrySize() const {
  return isAOT() ? sizeof(int32_t) : sizeof(uintptr_t);
}

void MacroAssembler::emitAOTStoreFrameTableBase(Register passReg,
                                                Register scratch,
                                                const Address& dst) {
  if (isAOT()) {
    // The entry preamble places the indirection table address in the register
    // used to initialize this frame.
    storePtr(passReg, dst);
    setAOTTableAddress(dst);
    return;
  }
  // All baseline frames store the indirection table address so static inline
  // cache stubs can access it.
  movePtr(ImmPtr(runtime()->jitRuntime()->aotIndirectionTable().baseAddress()),
          scratch);
  storePtr(scratch, dst);
}

void MacroAssembler::emitAOTCopyFrameTableBaseFromCaller(Register scratch) {
  Address tableAddress(FramePointer,
                       BaselineFrame::reverseOffsetOfAOTTableBase());
  setAOTTableAddress(tableAddress);
  // When resuming a generator, copy the indirection table address from the
  // caller's saved baseline frame into the reconstructed frame.
  loadPtr(Address(FramePointer, 0), scratch);
  loadPtr(Address(scratch, BaselineFrame::reverseOffsetOfAOTTableBase()),
          scratch);
  storePtr(scratch, tableAddress);
}

#endif  // ENABLE_JS_AOT
