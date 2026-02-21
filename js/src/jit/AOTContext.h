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

struct BaselineAOTAccumulator {
  using OffsetVector = Vector<uint32_t, 0, SystemAllocPolicy>;
  using RuntimePatchVector = Vector<RuntimePatch, 0, SystemAllocPolicy>;
  using ICReturnOffsetVector =
      Vector<BaselineInterpreter::ICReturnOffset, 0, SystemAllocPolicy>;

  BaselineManifest manifest = {};
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
  explicit AOTContext(TrampolinePtrs trampolines);

  // Bind this context to a MacroAssembler. Called by MacroAssembler's
  // constructor after it stores the AOTContext pointer.
  void bindMasm(MacroAssembler& masm) { masm_ = &masm; }

  const TrampolinePtrs& trampolines() const { return trampolines_; }

  bool zoneLoaded() const { return zoneLoaded_; }
  void setZoneLoaded() { zoneLoaded_ = true; }

  BaselineAOTAccumulator& accumulator() { return accumulator_; }

  void emitPatchableMovImm(RuntimePatch::Kind kind, Register dest);
  void emitVMWrapperPatchableMovImm(VMFunctionId id, Register dest);
  void emitCppFunctionPatchableMovImm(AOTCppFunctionId fnId, Register dest);
  void emitDebugTrapPatchableMovImm(DebugTrapHandlerKind dbgKind,
                                    Register dest);
  void emitSwitchToObjectRealm(Register obj, Register scratch,
                               Register realmDst);

 private:
  MacroAssembler* masm_ = nullptr;
  TrampolinePtrs trampolines_;
  bool zoneLoaded_ = false;
  BaselineAOTAccumulator accumulator_;
};

}  // namespace js::jit

#endif  // jit_AOTContext_h
