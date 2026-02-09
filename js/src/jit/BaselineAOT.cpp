/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"
#include <cstdint>
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/VMFunctions.h"
#include "mozilla/Assertions.h"
#include "vm/JSContext.h"

namespace js::jit {

static void* ResolveCppFunction(AOTCppFunctionId id) {
  switch (id) {
    case AOTCppFunctionId::PostWriteBarrier:
      return reinterpret_cast<void*>(PostWriteBarrier);
    case AOTCppFunctionId::FrameIsDebuggeeCheck:
      return reinterpret_cast<void*>(FrameIsDebuggeeCheck);
    case AOTCppFunctionId::HandleCodeCoverageAtPrologue:
      return reinterpret_cast<void*>(HandleCodeCoverageAtPrologue);
    case AOTCppFunctionId::HandleCodeCoverageAtPC:
      return reinterpret_cast<void*>(HandleCodeCoverageAtPC);
    case AOTCppFunctionId::Count:
      break;
  }
  MOZ_CRASH("Unknown AOTCppFunctionId");
}

uintptr_t RuntimePatch::_getValueToPatch(const PatchContext& pc) const {
  switch(kind) {
    case Kind::WellKnownSymbols:
      return (uintptr_t)pc.cx->runtime()->wellKnownSymbols.ref();
    case Kind::JitRuntime:
      return (uintptr_t)pc.cx->runtime()->jitRuntime();
    case Kind::ContextRealm:
      return (uintptr_t)(reinterpret_cast<const uint8_t*>(pc.cx)
              + JSContext::offsetOfRealm());
    case Kind::JSContextPtr:
      return (uintptr_t)pc.cx;
    case Kind::DispatchTable:
      return (uintptr_t)(pc.codeBase + handlerOffset);
    case Kind::VMWrapper: {
      TrampolinePtr ptr = pc.cx->runtime()->jitRuntime()->getVMWrapper(vmId);
      return (uintptr_t)(ptr.value);
    }
    case Kind::InterruptBits:
      return (uintptr_t)pc.cx->addressOfInterruptBits();
    case Kind::JitActivation:
      return (uintptr_t)pc.cx->addressOfJitActivation();
    case Kind::RealmPtr:
      return (uintptr_t)pc.cx->addressOfRealm();
    case Kind::LastBufferedCell:
      return (uintptr_t)pc.cx->runtime()->gc.addressOfLastBufferedWholeCell();
    case Kind::ProfilerEnabled:
      return (uintptr_t)pc.cx->runtime()->geckoProfiler().addressOfEnabled();
    case Kind::DebugTrapHandler:
      return (uintptr_t)pc.cx->runtime()->jitRuntime()->debugTrapHandler(dbgKind)->raw();
    case Kind::ProfilerExitFrameTail: {
      TrampolinePtr ptr = pc.cx->runtime()->jitRuntime()->getProfilerExitFrameTail();
      return (uintptr_t)(ptr.value);
    }
    case Kind::CppFunction:
      return (uintptr_t)ResolveCppFunction(cppFnId);
  }
  MOZ_CRASH("Unexpected Patch Type");
}

void RuntimePatch::apply(const PatchContext& pc) const {
  uintptr_t val = _getValueToPatch(pc);
  uint8_t* target = pc.codeBase + targetOffset;
#ifdef DEBUG
  uintptr_t beforeValue = *reinterpret_cast<uintptr_t*>(target);
  const char* kindStr = "Unknown";
  switch(kind) {
    case Kind::WellKnownSymbols: kindStr = "WellKnownSymbols"; break;
    case Kind::JitRuntime: kindStr = "JitRuntime"; break;
    case Kind::ContextRealm: kindStr = "ContextRealm"; break;
    case Kind::JSContextPtr: kindStr = "JSContextPtr"; break;
    case Kind::DispatchTable: kindStr = "DispatchTable"; break;
    case Kind::VMWrapper: kindStr = "VMWrapper"; break;
    case Kind::InterruptBits: kindStr = "InterruptBits"; break;
    case Kind::JitActivation: kindStr = "JitActivation"; break;
    case Kind::RealmPtr: kindStr = "RealmPtr"; break;
    case Kind::LastBufferedCell: kindStr = "LastBufferedCell"; break;
    case Kind::ProfilerEnabled: kindStr = "ProfilerEnabled"; break;
    case Kind::DebugTrapHandler: kindStr = "DebugTrapHandler"; break;
    case Kind::ProfilerExitFrameTail: kindStr = "ProfilerExitFrameTail"; break;
    case Kind::CppFunction: kindStr = "CppFunction"; break;
  }
  JitSpew(JitSpew_BaselineAOT, "Runtime patch [%s] @ offset %u: before=0x%016lx after=0x%016lx",
          kindStr, targetOffset, beforeValue, val);
#endif
  *reinterpret_cast<uintptr_t*>(target) = val;
}

}  // namespace js::jit
