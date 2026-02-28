/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTContext_h
#define jit_AOTContext_h

#include "jit/BaselineAOT.h"
#include "jit/BaselineJIT.h"
#include "jit/TrampolinePtrs.h"
#include "js/Vector.h"

namespace js::jit {

class MacroAssembler;
enum class DebugTrapHandlerKind;

// Strategy for resolving external references in AOT-compiled code.
enum class AOTStrategy {
  // Emit a sentinel immediate via movWithPatch and register a RuntimePatch
  // entry. At load time, the sentinel is replaced with the resolved address.
  // Used by the AOT baseline interpreter.
  PatchPointers,
  // Emit a register-indirect load chain: zone -> runtime -> field.
  // Used by AOT IC stubs.
  RegisterIndirect,
};

// Accumulate the metadata, required for reconstructing the BaselineInterpreter
// class at load time, and patches, to repair the AOT code at load time.
struct BaselineAOTAccumulator {
  using OffsetVector = Vector<uint32_t, 0, SystemAllocPolicy>;
  using RuntimePatchVector = Vector<RuntimePatch, 0, SystemAllocPolicy>;
  using ICReturnOffsetVector =
      Vector<BaselineInterpreter::ICReturnOffset, 0, SystemAllocPolicy>;

  OffsetVector debugInstr;
  OffsetVector debugTraps;
  OffsetVector codeCoverage;
  ICReturnOffsetVector icReturns;
  RuntimePatchVector runtimePatches;

  void registerPatch(RuntimePatch&& patch) {
    MOZ_ALWAYS_TRUE(runtimePatches.append(std::move(patch)));
  }
};

// Unified AOT compilation context. Stack-allocated by the caller and passed
// to MacroAssembler as a non-owning pointer. When present (non-nullptr),
// indicates that AOT codegen mode is active.
class AOTContext {
 public:
  AOTContext(AOTStrategy strategy, TrampolinePtrs trampolines);

  // Bind this context to a MacroAssembler. Called by MacroAssembler's
  // constructor after it stores the AOTContext pointer.
  void bindMasm(MacroAssembler& masm) { masm_ = &masm; }

  AOTStrategy strategy() const { return strategy_; }
  const TrampolinePtrs& trampolines() const { return trampolines_; }

  bool zoneLoaded() const { return zoneLoaded_; }
  void setZoneLoaded() { zoneLoaded_ = true; }

  BaselineAOTAccumulator& accumulator() { return accumulator_; }

  void emitPatchableMovImm(ExternalRefKind kind, Register dest);
  void emitVMWrapperPatchableMovImm(VMFunctionId id, Register dest);
  void emitCppFunctionPatchableMovImm(AOTCppFunctionId fnId, Register dest);
  void emitDebugTrapPatchableMovImm(DebugTrapHandlerKind dbgKind,
                                    Register dest);

 private:
  MacroAssembler* masm_ = nullptr;
  AOTStrategy strategy_;
  TrampolinePtrs trampolines_;
  bool zoneLoaded_ = false;
  BaselineAOTAccumulator accumulator_;
};

}  // namespace js::jit

#endif  // jit_AOTContext_h
