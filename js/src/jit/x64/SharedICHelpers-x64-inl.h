/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_SharedICHelpers_x64_inl_h
#define jit_x64_SharedICHelpers_x64_inl_h

#include "jit/BaselineFrame.h"
#include "jit/CompileWrappers.h"
#include "jit/JitRuntime.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm,
                                   uint32_t argSize) {
#ifdef DEBUG
  ScratchRegisterScope scratch(masm);

  // We can assume during this that R0 and R1 have been pushed.
  // Store frame size without VMFunction arguments for debug assertions.
  masm.movq(FramePointer, scratch);
  masm.subq(StackPointer, scratch);
  masm.subq(Imm32(argSize), scratch);
  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Push frame descriptor and perform the tail call.
  masm.push(FrameDescriptor(FrameType::BaselineJS));
  masm.push(ICTailCallReg);
  masm.jump(target);
}

inline void EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm) {
  masm.push(FrameDescriptor(FrameType::BaselineStub));
  masm.call(target);
}

inline void EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch) {
#ifdef DEBUG
  // Compute frame size. Because the return address is still on the stack,
  // this is:
  //
  //   FramePointer
  //   - StackPointer
  //   - sizeof(return address)

  masm.movq(FramePointer, scratch);
  masm.subq(StackPointer, scratch);
  masm.subq(Imm32(sizeof(void*)), scratch);  // Return address.

  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Push the return address that's currently on top of the stack.
  masm.Push(Operand(StackPointer, 0));

  // Replace the original return address with the frame descriptor.
  masm.storePtr(ImmWord(MakeFrameDescriptor(FrameType::BaselineJS)),
                Address(StackPointer, sizeof(uintptr_t)));

#ifdef ENABLE_JS_AOT
  // NOTE(aot): The AOT interpreter cannot bake the indirection table
  // address, so reload it from the baseline frame slot. Non-AOT callers
  // can move the compile-time base directly.
  if (masm.isAOT()) {
    masm.loadPtr(
        Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()),
        scratch);
  } else {
    const AOTIndirectionTable& table =
        masm.runtime()->jitRuntime()->aotIndirectionTable();
    masm.movePtr(ImmPtr(table.baseAddress()), scratch);
  }
#endif

  // Save old frame pointer, stack pointer and stub reg.
  masm.Push(FramePointer);
  masm.mov(StackPointer, FramePointer);

  masm.Push(ICStubReg);
#ifdef ENABLE_JS_AOT
  masm.Push(scratch);
#endif
}

}  // namespace jit
}  // namespace js

#endif /* jit_x64_SharedICHelpers_x64_inl_h */
