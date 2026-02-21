/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOTContext.h"

#include "jit/JitRuntime.h"
#include "vm/Shape.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

AOTContext::AOTContext(TrampolinePtrs trampolines)
    : trampolines_(trampolines) {}

void AOTContext::emitPatchableMovImm(RuntimePatch::Kind kind, Register dest) {
  CodeOffset off =
      masm_->movWithPatch(ImmPtr((void*)AOT_PATCH_SENTINEL), dest);
  uint32_t immOff = off.offset() - sizeof(void*);
  accumulator_.registerPatch(RuntimePatch(kind, immOff));
}

void AOTContext::emitVMWrapperPatchableMovImm(VMFunctionId id, Register dest) {
  CodeOffset off =
      masm_->movWithPatch(ImmPtr((void*)AOT_PATCH_SENTINEL), dest);
  uint32_t immOff = off.offset() - sizeof(void*);
  accumulator_.registerPatch(RuntimePatch::VMWrapperPatch(immOff, id));
}

void AOTContext::emitCppFunctionPatchableMovImm(AOTCppFunctionId fnId,
                                                Register dest) {
  CodeOffset off =
      masm_->movWithPatch(ImmPtr((void*)AOT_PATCH_SENTINEL), dest);
  uint32_t immOff = off.offset() - sizeof(void*);
  accumulator_.registerPatch(RuntimePatch::CppFunctionPatch(immOff, fnId));
}

void AOTContext::emitDebugTrapPatchableMovImm(DebugTrapHandlerKind dbgKind,
                                              Register dest) {
  CodeOffset off =
      masm_->movWithPatch(ImmPtr((void*)AOT_PATCH_SENTINEL), dest);
  uint32_t immOff = off.offset() - sizeof(void*);
  accumulator_.registerPatch(RuntimePatch::DebugTrapPatch(immOff, dbgKind));
}

void AOTContext::emitSwitchToObjectRealm(Register obj, Register scratch,
                                         Register realmDst) {
  masm_->loadObjShapeUnsafe(obj, scratch);
  masm_->loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  masm_->loadPtr(Address(scratch, BaseShape::offsetOfRealm()), scratch);
  emitPatchableMovImm(RuntimePatch::Kind::ContextRealm, realmDst);
  masm_->storePtr(scratch, Address(realmDst, 0));
}
