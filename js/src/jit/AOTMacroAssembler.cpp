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

#include "jit/JitRuntime.h"
#include "jit/VMFunctions.h"
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::jit;

#ifdef ENABLE_AOT_BASELINE

// Compute the offset of JSContext within TLS storage. This offset is
// baked into AOT code so that it can reach the runtime without any
// relocatable pointer.
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

// Load a value from the AOT indirection table via:
//   TlsContext (%fs:off) -> cx->runtime_ -> jitRuntime_ -> slots_[slot]
static void EmitAOTSlotLoad(MacroAssembler& masm, AOTSlot slot,
                            Register dest) {
  int32_t tlsOff = GetTlsContextOffset();
  masm.loadPtrFromTls(tlsOff, dest);
  masm.loadPtr(Address(dest, JSContext::offsetOfRuntime()), dest);
  masm.loadPtr(Address(dest, JSRuntime::offsetOfJitRuntime()), dest);
  int32_t tableOff = int32_t(JitRuntime::offsetOfAOTIndirectionTable()) +
                     int32_t(AOTIndirectionTable::offsetOfSlot(slot));
  masm.loadPtr(Address(dest, tableOff), dest);
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

// ========================================================================
// AOT slot primitives.
// These are the building blocks that BaselineCodeGen.cpp calls.

void* MacroAssembler::aotSlotAddress(AOTSlot slot) {
  return (void*)runtime()->jitRuntime()->aotIndirectionTable().get(slot);
}

void MacroAssembler::moveAOTSlot(AOTSlot slot, Register dest) {
  if (!isAOT()) {
    movePtr(ImmPtr(aotSlotAddress(slot)), dest);
    return;
  }
  EmitAOTSlotLoad(*this, slot, dest);
}

void MacroAssembler::loadAOTSlot(AOTSlot slot, Register dest) {
  moveAOTSlot(slot, dest);
  loadPtr(Address(dest, 0), dest);
}

void MacroAssembler::branchAOTSlot32(Condition cond, AOTSlot slot, Imm32 rhs,
                                     Label* label, Register scratch) {
  if (!isAOT()) {
    branch32(cond, AbsoluteAddress(aotSlotAddress(slot)), rhs, label);
    return;
  }
  EmitAOTSlotLoad(*this, slot, scratch);
  branch32(cond, Address(scratch, 0), rhs, label);
}

void MacroAssembler::callPreBarrierAOT(MIRType type, Register scratch) {
  MOZ_ASSERT(isAOT());
  MOZ_ASSERT(scratch != PreBarrierReg);
  EmitAOTSlotLoad(*this, PreBarrierSlotForMIRType(type), scratch);
  call(scratch);
}

#endif

void MacroAssembler::loadAOTRuntime(Register reg) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    EmitAOTSlotLoad(*this, AOTSlot::JSRuntimePtr, reg);
    return;
  }
#endif
  movePtr(ImmPtr(runtime()), reg);
}

void MacroAssembler::loadAOTVMWrapper(VMFunctionId id, Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    EmitAOTSlotLoad(*this, AOTSlot::VMWrapperBase, dest);
    loadPtr(Address(dest, size_t(id) * sizeof(uintptr_t)), dest);
    return;
  }
#endif
  TrampolinePtr ptr = runtime()->jitRuntime()->getVMWrapper(id);
  movePtr(ImmPtr(ptr.value), dest);
}

void MacroAssembler::loadAOTDebugTrapHandler(DebugTrapHandlerKind dbgKind,
                                             Register dest) {
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    AOTSlot slot = dbgKind == DebugTrapHandlerKind::Interpreter
                       ? AOTSlot::DebugTrapInterpreter
                       : AOTSlot::DebugTrapCompiler;
    EmitAOTSlotLoad(*this, slot, dest);
    return;
  }
#endif
  JitCode* handler = runtime()->jitRuntime()->debugTrapHandler(dbgKind);
  movePtr(ImmPtr(handler->raw()), dest);
}

void MacroAssembler::writeDispatchTableEntry(uint32_t tableOffset,
                                              size_t index,
                                              const Label& handler) {
  MOZ_ASSERT(handler.bound());
#ifdef ENABLE_AOT_BASELINE
  if (isAOT()) {
    // AOT mode: emit a position-independent int32 offset relative to the
    // table base.  No runtime patch needed; the offset is baked in.
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
