/* -*- Mode: C++; tab-width: 8; indent-style-mode: nil; c-basic-offset: 2 -*-
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

// NOTE(Justin): x86-64 only for now (%fs segment).
static int32_t GetTlsContextOffset() {
  uintptr_t tp;
  asm("movq %%fs:0, %0" : "=r"(tp));
  auto offset =
      reinterpret_cast<intptr_t>(&TlsContext) - static_cast<intptr_t>(tp);
  MOZ_ASSERT(offset == static_cast<int32_t>(offset),
             "TLS offset must fit in int32_t");
  return static_cast<int32_t>(offset);
}

// Load an AOT slot value via the cached table base in BaselineFrame.
// 2 loads: FramePointer[aotTableBase_] -> slots_[slot]
void MacroAssembler::emitAOTSlotLoad(AOTSlot slot, Register dest) {
  MacroAssemblerSpecific::loadPtr(
      Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()), dest);
#ifdef DEBUG
  {
    Label ok;
    branchPtr(Assembler::NotEqual, dest, FramePointer, &ok);
    assumeUnreachable("aotTableBase_ == FramePointer frame field corrupted");
    bind(&ok);
  }
#endif
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

void MacroAssembler::loadAOTTableBase(Register dest) {
  int32_t tlsOff = GetTlsContextOffset();
  loadPtrFromTls(tlsOff, dest);
  MacroAssemblerSpecific::loadPtr(Address(dest, JSContext::offsetOfRuntime()), dest);
  MacroAssemblerSpecific::loadPtr(Address(dest, JSRuntime::offsetOfJitRuntime()), dest);
  addPtr(Imm32(int32_t(JitRuntime::offsetOfAOTIndirectionTable())), dest);
}

void MacroAssembler::storeAOTTableBaseToFrame(Register scratch) {
  loadAOTTableBase(scratch);
  storePtr(scratch, Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()));
}

#endif  // ENABLE_AOT_BASELINE

// ========================================================================
// AOT-transparent overrides.
//
// movePtr(ImmPtr) and loadPtr(AbsoluteAddress) are overridden so that
// in AOT mode, any pointer value that matches a known indirection table
// slot is automatically emitted as a TLS-based load.  This makes AOT
// codegen transparent to callers.

void MacroAssembler::movePtr(ImmPtr imm, Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    auto slot = aot().indirectionTable()->findSlot(uintptr_t(imm.value));
    if (slot) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

void MacroAssembler::loadPtr(AbsoluteAddress addr, Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    auto slot = aot().indirectionTable()->findSlot(uintptr_t(addr.addr));
    if (slot) {
      emitAOTSlotLoad(*slot, dest);
      MacroAssemblerSpecific::loadPtr(Address(dest, 0), dest);
      return;
    }
  }
#endif
  MacroAssemblerSpecific::loadPtr(addr, dest);
}

// ========================================================================
// Dual-mode helpers.

void MacroAssembler::loadRuntime(Register reg) {
  movePtr(ImmPtr(runtime()), reg);
}

void MacroAssembler::loadVMWrapper(VMFunctionId id, Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    emitAOTSlotLoad(AOTSlot::VMWrapperBase, dest);
    MacroAssemblerSpecific::loadPtr(Address(dest, size_t(id) * sizeof(uintptr_t)), dest);
    return;
  }
#endif
  TrampolinePtr ptr = runtime()->jitRuntime()->getVMWrapper(id);
  movePtr(ImmPtr(ptr.value), dest);
}

void MacroAssembler::loadDebugTrapHandler(DebugTrapHandlerKind dbgKind,
                                          Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    AOTSlot slot = dbgKind == DebugTrapHandlerKind::Interpreter
                       ? AOTSlot::DebugTrapInterpreter
                       : AOTSlot::DebugTrapCompiler;
    emitAOTSlotLoad(slot, dest);
    return;
  }
#endif
  JitCode* handler = runtime()->jitRuntime()->debugTrapHandler(dbgKind);
  movePtr(ImmPtr(handler->raw()), dest);
}

// Rather than clutter BaselineCodeGen with a bifurcated ifdef, we abstract,
// even though there is only one use, the logic for filling the blinterp
// disptach table.
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
