/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTMacroAssembler_inl_h
#define jit_AOTMacroAssembler_inl_h

#ifdef ENABLE_JS_AOT

#  include "jit/CompileWrappers.h"
#  include "jit/MacroAssembler.h"

namespace js::jit {

// Values below this threshold represent sentinels rather than runtime
// addresses, so encode them directly.
static constexpr uintptr_t kAOTBakeableSentinelLimit = 16;

static inline void AssertAOTPointerIsBakeable(uintptr_t value,
                                              const char* operation) {
  if (value >= kAOTBakeableSentinelLimit) {
    MOZ_CRASH_UNSAFE_PRINTF(
        "AOT: no indirection slot for %s %p, "
        "add to the AOT indirection table.",
        operation, reinterpret_cast<void*>(value));
  }
}

inline void MacroAssembler::loadRuntime(Register reg) {
  movePtr(ImmPtr(runtime()), reg);
}

inline void MacroAssembler::movePtr(TrampolinePtr ptr, Register dest) {
  movePtr(ImmPtr(ptr.value), dest);
}

inline void MacroAssembler::movePtr(ImmPtr imm, Register dest) {
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    if (auto slot = aotTable().findSlot(val)) {
      emitAOTAddress(*slot, dest);
      return;
    }
    AssertAOTPointerIsBakeable(val, "movePtr(ImmPtr)");
  }
  MacroAssemblerSpecific::movePtr(imm, dest);
}

inline void MacroAssembler::movePtr(ImmGCPtr imm, Register dest) {
  if (MOZ_UNLIKELY(isAOT())) {
    if (auto slot = aotTable().findAtomSlot(uintptr_t(imm.value))) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
    // Other GC pointers retain their normal relocation behavior.
  }
  MacroAssemblerSpecific::movePtr(imm, dest);
}

#  ifndef JS_CODEGEN_RISCV64
inline void MacroAssembler::loadPtr(AbsoluteAddress addr, Register dest) {
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(addr.addr);
    if (auto slot = aotTable().findSlot(val)) {
      if (IsAOTLinkSlot(*slot)) {
        emitAOTLinkLoad(*slot, dest);
      } else {
        emitAOTSlotLoad(*slot, dest);
        MacroAssemblerSpecific::loadPtr(Address(dest, 0), dest);
      }
      return;
    }
    AssertAOTPointerIsBakeable(val, "loadPtr(AbsoluteAddress)");
  }
  MacroAssemblerSpecific::loadPtr(addr, dest);
}
#  endif

inline void MacroAssembler::storePtr(ImmPtr imm, const Address& address) {
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    if (auto slot = aotTable().findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTAddress(*slot, scratch);
      MacroAssemblerSpecific::storePtr(scratch, address);
      return;
    }
    AssertAOTPointerIsBakeable(val, "storePtr(ImmPtr, Address)");
  }
  MacroAssemblerSpecific::storePtr(imm, address);
}

#  ifndef JS_CODEGEN_RISCV64
inline void MacroAssembler::storePtr(Register src, AbsoluteAddress address) {
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(address.addr);
    if (auto slot = aotTable().findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTAddress(*slot, scratch);
      MacroAssemblerSpecific::storePtr(src, Address(scratch, 0));
      return;
    }
    AssertAOTPointerIsBakeable(val, "storePtr(Register, AbsoluteAddress)");
  }
  MacroAssemblerSpecific::storePtr(src, address);
}
#  endif

inline void MacroAssembler::jump(TrampolinePtr code) {
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(code.value);
    if (auto slot = aotTable().findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTSlotJump(*slot, scratch);
      return;
    }
    AssertAOTPointerIsBakeable(val, "jump(TrampolinePtr)");
  }
  MacroAssemblerSpecific::jump(code);
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTMacroAssembler_inl_h
