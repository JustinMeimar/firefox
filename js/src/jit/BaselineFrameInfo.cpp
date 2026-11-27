/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrameInfo.h"

#include <algorithm>

#include "jit/BaselineIC.h"
#ifdef DEBUG
#  include "jit/BytecodeAnalysis.h"
#endif

#include "jit/JitFrames.h"

#include "jit/BaselineFrameInfo-inl.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

bool CompilerFrameInfo::init(TempAllocator& alloc) {
  // An extra slot is needed for global scopes because INITGLEXICAL (stack
  // depth 1) is compiled as a SETPROP (stack depth 2) on the global lexical
  // scope.
  size_t extra = script->isGlobalCode() ? 1 : 0;
  size_t nstack =
      std::max(script->nslots() - script->nfixed(), size_t(MinJITStackSize)) +
      extra;
  if (!stack.init(alloc, nstack)) {
    return false;
  }

  return true;
}

bool CompilerFrameInfo::initSlotTypes(TempAllocator& alloc) {
  size_t nargs = script->isFunction() ? this->nargs() : 0;
  if (!slotTypes_.init(alloc, nlocals() + nargs + 1)) {
    return false;
  }
  alloc_ = &alloc;
  forgetSlotTypes();
  return true;
}

void CompilerFrameInfo::forgetSlotTypes() {
  for (size_t i = 0; i < slotTypes_.length(); i++) {
    slotTypes_[i] = JSVAL_TYPE_UNKNOWN;
  }
}

bool CompilerFrameInfo::mergeSlotTypes(uint32_t targetOffset) {
  if (!tracksSlotTypes()) {
    return true;
  }
  MergeStateMap::AddPtr p = mergeStates_.lookupForAdd(targetOffset);
  if (p) {
    JSValueType* state = p->value();
    for (size_t i = 0; i < slotTypes_.length(); i++) {
      if (state[i] != slotTypes_[i]) {
        state[i] = JSVAL_TYPE_UNKNOWN;
      }
    }
    return true;
  }
  JSValueType* state = alloc_->allocateArray<JSValueType>(slotTypes_.length());
  if (!state) {
    return false;
  }
  std::copy(slotTypes_.begin(), slotTypes_.end(), state);
  return mergeStates_.add(p, targetOffset, state);
}

void CompilerFrameInfo::enterJumpTarget(uint32_t offset, bool allEdgesMerged) {
  for (size_t i = 0; i < stackDepth(); i++) {
    if (stack[i].kind() == StackValue::Stack) {
      stack[i].setStack();
    }
  }
  if (!tracksSlotTypes()) {
    return;
  }
  MergeStateMap::Ptr p = mergeStates_.lookup(offset);
  if (p && allEdgesMerged) {
    std::copy(p->value(), p->value() + slotTypes_.length(), slotTypes_.begin());
  } else {
    forgetSlotTypes();
  }
  if (p) {
    mergeStates_.remove(p);
  }
}

void CompilerFrameInfo::moveSyncedValue(int32_t from, int32_t to) {
  StackValue* source = peek(from);
  StackValue* dest = peek(to);
  MOZ_ASSERT(source->kind() == StackValue::Stack);
  MOZ_ASSERT(dest->kind() == StackValue::Stack);
  if (source < dest) {
    std::rotate(source, source + 1, dest + 1);
  } else {
    std::rotate(dest, source, source + 1);
  }
}

void CompilerFrameInfo::storeLocalType(uint32_t local) {
  if (tracksSlotTypes()) {
    const StackValue* val = peek(-1);
    localType(local) =
        val->hasKnownType() ? val->knownType() : JSVAL_TYPE_UNKNOWN;
  }
}

void CompilerFrameInfo::storeArgType(uint32_t arg) {
  if (tracksSlotTypes()) {
    const StackValue* val = peek(-1);
    argType(arg) = val->hasKnownType() ? val->knownType() : JSVAL_TYPE_UNKNOWN;
  }
}

void CompilerFrameInfo::setSlotType(const StackValue& val, JSValueType type) {
  if (!tracksSlotTypes()) {
    return;
  }
  switch (val.kind()) {
    case StackValue::LocalSlot:
      localType(val.localSlot()) = type;
      break;
    case StackValue::ArgSlot:
      argType(val.argSlot()) = type;
      break;
    case StackValue::ThisSlot:
      thisType() = type;
      break;
    default:
      break;
  }
}

void CompilerFrameInfo::pushValue(const StackValue& val) {
  switch (val.kind()) {
    case StackValue::Stack:
      break;
    case StackValue::LocalSlot:
      masm.pushValue(addressOfLocal(val.localSlot()));
      break;
    case StackValue::ArgSlot:
      masm.pushValue(addressOfArg(val.argSlot()));
      break;
    case StackValue::ThisSlot:
      masm.pushValue(addressOfThis());
      break;
    case StackValue::Register:
      masm.pushValue(val.reg());
      break;
    case StackValue::Constant:
      masm.pushValue(val.constant());
      break;
    default:
      MOZ_CRASH("Invalid kind");
  }
}

void CompilerFrameInfo::sync(StackValue* val) {
  pushValue(*val);
  val->setStack(val->hasKnownType() ? val->knownType() : JSVAL_TYPE_UNKNOWN);
}

void CompilerFrameInfo::syncStack(uint32_t uses) {
  MOZ_ASSERT(uses <= stackDepth());

  uint32_t depth = stackDepth() - uses;
  if (deferSync_) {
    while (depth > 0 && stack[depth - 1].kind() != StackValue::Register) {
      depth--;
    }
  }

  for (uint32_t i = 0; i < depth; i++) {
    StackValue* current = &stack[i];
    sync(current);
  }
}

uint32_t CompilerFrameInfo::numUnsyncedSlots() {
  // Start at the bottom, find the first value that's not synced.
  uint32_t i = 0;
  for (; i < stackDepth(); i++) {
    if (peek(-int32_t(i + 1))->kind() == StackValue::Stack) {
      break;
    }
  }
  return i;
}

void CompilerFrameInfo::popValue(ValueOperand dest) {
  StackValue* val = peek(-1);

  switch (val->kind()) {
    case StackValue::Constant:
      masm.moveValue(val->constant(), dest);
      break;
    case StackValue::LocalSlot:
      masm.loadValue(addressOfLocal(val->localSlot()), dest);
      break;
    case StackValue::ArgSlot:
      masm.loadValue(addressOfArg(val->argSlot()), dest);
      break;
    case StackValue::ThisSlot:
      masm.loadValue(addressOfThis(), dest);
      break;
    case StackValue::Stack:
      masm.popValue(dest);
      break;
    case StackValue::Register:
      masm.moveValue(val->reg(), dest);
      break;
    default:
      MOZ_CRASH("Invalid kind");
  }

  // masm.popValue already adjusted the stack pointer, don't do it twice.
  pop(DontAdjustStack);
}

void CompilerFrameInfo::popRegsAndSync(uint32_t uses) {
  // x86 has only 3 Value registers. Only support 2 regs here for now,
  // so that there's always a scratch Value register for reg -> reg
  // moves.
  MOZ_ASSERT(uses > 0);
  MOZ_ASSERT(uses <= 2);
  MOZ_ASSERT(uses <= stackDepth());

  syncStack(uses);

  switch (uses) {
    case 1:
      popValue(R0);
      break;
    case 2: {
      // If the second value is in R1, move it to R2 so that it's not
      // clobbered by the first popValue.
      StackValue* val = peek(-2);
      if (val->kind() == StackValue::Register && val->reg() == R1) {
        masm.moveValue(R1, ValueOperand(R2));
        val->setRegister(
            R2, val->hasKnownType() ? val->knownType() : JSVAL_TYPE_UNKNOWN);
      }
      popValue(R1);
      popValue(R0);
      break;
    }
    default:
      MOZ_CRASH("Invalid uses");
  }
  // On arm64, SP may be < PSP now (that's OK).
  // eg testcase: tests/bug1580246.js
}

void InterpreterFrameInfo::popRegsAndSync(uint32_t uses) {
  switch (uses) {
    case 1:
      popValue(R0);
      break;
    case 2: {
      popValue(R1);
      popValue(R0);
      break;
    }
    default:
      MOZ_CRASH("Invalid uses");
  }
  // On arm64, SP may be < PSP now (that's OK).
  // eg testcase: tests/backup-point-bug1315634.js
}

void InterpreterFrameInfo::bumpInterpreterICEntry() {
  masm.addPtr(Imm32(sizeof(ICEntry)), addressOfInterpreterICEntry());
}

void CompilerFrameInfo::storeStackValue(int32_t depth, const Address& dest,
                                        const ValueOperand& scratch) {
  const StackValue* source = peek(depth);
  switch (source->kind()) {
    case StackValue::Constant:
      masm.storeValue(source->constant(), dest);
      break;
    case StackValue::Register:
      masm.storeValue(source->reg(), dest);
      break;
    case StackValue::LocalSlot:
      masm.loadValue(addressOfLocal(source->localSlot()), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::ArgSlot:
      masm.loadValue(addressOfArg(source->argSlot()), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::ThisSlot:
      masm.loadValue(addressOfThis(), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::Stack:
      masm.loadValue(addressOfStackValue(depth), scratch);
      masm.storeValue(scratch, dest);
      break;
    default:
      MOZ_CRASH("Invalid kind");
  }
}

#ifdef DEBUG
void CompilerFrameInfo::assertValidState(const BytecodeInfo& info) {
  // Check stack depth.
  MOZ_ASSERT(stackDepth() == info.stackDepth);

  // Start at the bottom, find the first value that's not synced.
  uint32_t i = 0;
  for (; i < stackDepth(); i++) {
    if (stack[i].kind() != StackValue::Stack) {
      break;
    }
  }

  // Assert all values on top of it are also not synced.
  for (; i < stackDepth(); i++) {
    MOZ_ASSERT(stack[i].kind() != StackValue::Stack);
  }

  // Assert every Value register is used by at most one StackValue.
  // R2 is used as scratch register by the compiler and FrameInfo,
  // so it shouldn't be used for StackValues.
  bool usedR0 = false, usedR1 = false;

  for (i = 0; i < stackDepth(); i++) {
    if (stack[i].kind() == StackValue::Register) {
      ValueOperand reg = stack[i].reg();
      if (reg == R0) {
        MOZ_ASSERT(!usedR0);
        usedR0 = true;
      } else if (reg == R1) {
        MOZ_ASSERT(!usedR1);
        usedR1 = true;
      } else {
        MOZ_CRASH("Invalid register");
      }
    }
  }
}
#endif
