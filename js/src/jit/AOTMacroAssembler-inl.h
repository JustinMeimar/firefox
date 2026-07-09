/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTMacroAssembler_inl_h
#define jit_AOTMacroAssembler_inl_h

#include "jit/CompileWrappers.h"
#include "jit/MacroAssembler.h"

namespace js::jit {

#ifdef ENABLE_JS_AOT
// NOTE(aot): only sub-page sentinel values (val < 16) are safe to bake as
// literal pointers. Real user-space pointers on Linux can never land there
// (page 0 is unmappable), so the SpecialScriptBit sentinels 0x1/0x3/0x5 pass
// while anything that could be a real address is rejected. Genuine small
// bit-patterns should use ImmWord; real pointers must be added to the
// indirection table.
static constexpr uintptr_t kAOTBakeableSentinelLimit = 16;

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

inline void MacroAssembler::loadRuntime(Register reg) {
  movePtr(ImmPtr(runtime()), reg);
}

inline void MacroAssembler::loadZoneBase(Register dest) {
#ifdef ENABLE_JS_AOT
  if (isAOT()) {
    loadZoneForAOT(dest);
    return;
  }
#endif
  MacroAssemblerSpecific::movePtr(ImmPtr(realm()->zone()->zone()), dest);
}

inline void MacroAssembler::movePtr(ImmPtr imm, Register dest) {
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

inline void MacroAssembler::movePtr(ImmGCPtr imm, Register dest) {
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
inline void MacroAssembler::loadPtr(AbsoluteAddress addr, Register dest) {
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

inline void MacroAssembler::storePtr(ImmPtr imm, const Address& address) {
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
inline void MacroAssembler::storePtr(Register src, AbsoluteAddress address) {
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

inline void MacroAssembler::jump(TrampolinePtr code) {
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

// NOTE(aot): entries are PIC-friendly int32 offsets in AOT mode, absolute
// CodeLabel pointers otherwise.
inline void MacroAssembler::writeDispatchTableEntry(uint32_t tableOffset,
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

#ifdef ENABLE_JS_AOT
#  undef AOT_CRASH_ON_UNKNOWN_PTR
#endif

}  // namespace js::jit

#endif  // jit_AOTMacroAssembler_inl_h
