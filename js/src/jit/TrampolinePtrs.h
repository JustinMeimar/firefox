/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TrampolinePtrs_h
#define jit_TrampolinePtrs_h

#include "jit/JitRuntime.h"
#include "jit/shared/Assembler-shared.h"

namespace js::jit {

// A simple collection of trampoline ptrs required during AOT compilation of
// ICs.
// The MacroAssembler could depend on this struct only instead of the runtime,
// which would not exist during AOT IC compilation.
// N.B: It is assumed that the trampolines are also generated AOT.
struct TrampolinePtrs {
  const TrampolinePtr exceptionTail;
  const TrampolinePtr valuePreBarrier;
  const TrampolinePtr stringPreBarrier;
  const TrampolinePtr objectPreBarrier;
  const TrampolinePtr shapePreBarrier;
  const TrampolinePtr wasmAnyRefPreBarrier;

  TrampolinePtrs() = delete;
  // N.B: This constructor should change accordingly when trampolines are
  // AOT generated. In that case, it would not be taking in JitRuntime.
  // At the moment, it suffices to construct this struct using JitRuntime, as
  // AOT IC compilation has not been decoupled into its own build phase yet.
  TrampolinePtrs(const JitRuntime* jitRuntime) :
    exceptionTail(jitRuntime->getExceptionTail()),
    valuePreBarrier(jitRuntime->preBarrier(MIRType::Value)),
    stringPreBarrier(jitRuntime->preBarrier(MIRType::String)),
    objectPreBarrier(jitRuntime->preBarrier(MIRType::Object)),
    shapePreBarrier(jitRuntime->preBarrier(MIRType::Shape)),
    wasmAnyRefPreBarrier(jitRuntime->preBarrier(MIRType::WasmAnyRef))
    {}

  TrampolinePtr preBarrier(MIRType type) const {
    switch (type) {
      case MIRType::Value:
        return valuePreBarrier;
      case MIRType::String:
        return stringPreBarrier;
      case MIRType::Object:
        return objectPreBarrier;
      case MIRType::Shape:
        return shapePreBarrier;
      case MIRType::WasmAnyRef:
        return wasmAnyRefPreBarrier;
      default:
        MOZ_CRASH();
    }
  }
};

}

#endif