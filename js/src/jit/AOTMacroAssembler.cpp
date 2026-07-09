/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/VMFunctions.h"
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSObject-inl.h"

// NOTE(Justin): This file is separate from MacroAssembler.cpp purely for
// convenience. The interfaces here all belong to `MacroAssembler` and
// could be moved into the primary source file eventually.

using namespace js;
using namespace js::jit;

#ifdef ENABLE_JS_AOT

void MacroAssembler::emitAOTSlotLoad(AOTSlot slot, Register dest) {
  // NOTE(aot): stub frames get the table base pushed by EmitBaselineEnterStubFrame.
  if (inAOTStubFrame_) {
    MacroAssemblerSpecific::loadPtr(
        Address(FramePointer, BaselineStubFrameLayout::AOTTableOffsetFromFP),
        dest);
  } else {
    MacroAssemblerSpecific::loadPtr(
        Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()),
        dest);
  }
  int32_t slotOff = int32_t(AOTIndirectionTable::offsetOfSlot(slot));
  MacroAssemblerSpecific::loadPtr(Address(dest, slotOff), dest);
}

static AOTSlot PreBarrierSlotForMIRType(MIRType type) {
  // NOTE(aot): mirrors JitRuntime::preBarrier but for AOT slots; trampolineCode
  // ptrs are runtime-generated and can't be called directly in AOT code.
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

void MacroAssembler::emitAOTStoreFrameTableBase(Register passReg,
                                                Register scratch,
                                                const Address& dst) {
  if (MOZ_UNLIKELY(isAOT())) {
    storePtr(passReg, dst);
  } else if (JitOptions.aotNeedsIndirectionTable()) {
    movePtr(ImmPtr(runtime()
                       ->jitRuntime()
                       ->aotIndirectionTable()
                       .baseAddress()),
            scratch);
    storePtr(scratch, dst);
  }
}

void MacroAssembler::emitAOTCopyFrameTableBaseFromCaller(Register scratch) {
  if (!isAOT() && !JitOptions.aotNeedsIndirectionTable()) {
    return;
  }
  // Assumes FramePointer points at the current BaselineFrame and the
  // caller's FP is at [FramePointer + 0].
  loadPtr(Address(FramePointer, 0), scratch);
  loadPtr(Address(scratch, BaselineFrame::reverseOffsetOfAOTTableBase()),
          scratch);
  storePtr(scratch,
           Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()));
}

void MacroAssembler::emitAOTDispatch(Register opcodeReg, Register tableReg) {
  if (isAOT()) {
    // Table entries are int32 offsets; add to tableReg to recover an
    // absolute address.
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

#endif  // ENABLE_JS_AOT

void MacroAssembler::loadRuntime(Register reg) {
  movePtr(ImmPtr(runtime()), reg);
}

void MacroAssembler::loadZoneBase(Register dest) {
#ifdef ENABLE_JS_AOT
  if (isAOT()) {
    loadZoneForAOT(dest);
    return;
  }
#endif
  MacroAssemblerSpecific::movePtr(ImmPtr(realm()->zone()->zone()), dest);
}

#ifdef ENABLE_JS_AOT
// NOTE(aot): only sub-page sentinel values (val < 16) are safe to bake as
// literal pointers. Real user-space pointers on Linux can never land there
// (page 0 is unmappable), so the SpecialScriptBit sentinels 0x1/0x3/0x5 pass
// while anything that could be a real address is rejected. Genuine small
// bit-patterns should use ImmWord; real pointers must be added to the
// indirection table.
//
// What to do about OSX and Windows?
static constexpr uintptr_t kAOTBakeableSentinelLimit = 16;

// Crash when an AOT intercept sees an unknown pointer that isn't a small
// sentinel. Real pointers must be registered in the indirection table.
#  define AOT_CRASH_ON_UNKNOWN_PTR(kind, val)                             \
    do {                                                                  \
      if ((val) >= kAOTBakeableSentinelLimit) {                           \
        MOZ_CRASH_UNSAFE_PRINTF(                                          \
            "AOT: no indirection slot for " kind                          \
            " %p -- add to the AOT indirection table.",                   \
            reinterpret_cast<void*>(val));                                \
      }                                                                   \
    } while (0)
#endif

void MacroAssembler::movePtr(ImmPtr imm, Register dest) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    if (auto slot = aot().indirectionTable()->findSlot(val)) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("movePtr(ImmPtr)", val);
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

void MacroAssembler::movePtr(ImmGCPtr imm, Register dest) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    if (auto slot = aot().indirectionTable()->findSlot(uintptr_t(imm.value))) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
    // NOTE(aot): unknown GC ptrs (e.g. script-specific atoms) fall through
    // to a normal relocatable move.
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

#ifndef JS_CODEGEN_RISCV64
void MacroAssembler::loadPtr(AbsoluteAddress addr, Register dest) {
#  ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(addr.addr);
    if (auto slot = aot().indirectionTable()->findSlot(val)) {
      emitAOTSlotLoad(*slot, dest);
      MacroAssemblerSpecific::loadPtr(Address(dest, 0), dest);
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("loadPtr(AbsoluteAddress)", val);
  }
#  endif
  MacroAssemblerSpecific::loadPtr(addr, dest);
}
#endif

void MacroAssembler::storePtr(ImmPtr imm, const Address& address) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    if (auto slot = aot().indirectionTable()->findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTSlotLoad(*slot, scratch);
      MacroAssemblerSpecific::storePtr(scratch, address);
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("storePtr(ImmPtr, Address)", val);
  }
#endif
  MacroAssemblerSpecific::storePtr(imm, address);
}

#ifndef JS_CODEGEN_RISCV64
void MacroAssembler::storePtr(Register src, AbsoluteAddress address) {
#  ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(address.addr);
    if (auto slot = aot().indirectionTable()->findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTSlotLoad(*slot, scratch);
      MacroAssemblerSpecific::storePtr(src, Address(scratch, 0));
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("storePtr(Register, AbsoluteAddress)", val);
  }
#  endif
  MacroAssemblerSpecific::storePtr(src, address);
}
#endif

void MacroAssembler::jump(TrampolinePtr code) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(code.value);
    if (auto slot = aot().indirectionTable()->findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTSlotLoad(*slot, scratch);
      MacroAssemblerSpecific::jump(scratch);
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("jump(TrampolinePtr)", val);
  }
#endif
  MacroAssemblerSpecific::jump(code);
}

#ifdef ENABLE_JS_AOT
#  undef AOT_CRASH_ON_UNKNOWN_PTR
#endif

// NOTE(aot): entries are PIC-friendly int32 offsets in AOT mode, absolute
// CodeLabel pointers otherwise.
void MacroAssembler::writeDispatchTableEntry(uint32_t tableOffset,
                                              size_t index,
                                              const Label& handler) {
  MOZ_ASSERT(handler.bound());
#ifdef ENABLE_JS_AOT
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
