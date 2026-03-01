/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOTReloc.h"

#include "jit/JitRuntime.h"
#include "vm/Caches.h"
#include "vm/JSContext.h"

namespace js::jit {

uintptr_t ResolveAOTReloc(AOTRelocKind kind, JSContext* cx) {
  switch (kind) {
    case AOTRelocKind::JSContextPtr:
      return (uintptr_t)cx;
    case AOTRelocKind::InterruptBits:
      return (uintptr_t)cx->addressOfInterruptBits();
    case AOTRelocKind::JitActivation:
      return (uintptr_t)cx->addressOfJitActivation();
    case AOTRelocKind::RealmPtr:
      return (uintptr_t)cx->addressOfRealm();
    case AOTRelocKind::ContextRealm:
      return (uintptr_t)(reinterpret_cast<const uint8_t*>(cx) +
                         JSContext::offsetOfRealm());
    case AOTRelocKind::WellKnownSymbols:
      return (uintptr_t)cx->runtime()->wellKnownSymbols.ref();
    case AOTRelocKind::JitRuntime:
      return (uintptr_t)cx->runtime()->jitRuntime();
    case AOTRelocKind::LastBufferedCell:
      return (uintptr_t)cx->runtime()->gc.addressOfLastBufferedWholeCell();
    case AOTRelocKind::ProfilerEnabled:
      return (uintptr_t)cx->runtime()->geckoProfiler().addressOfEnabled();
    case AOTRelocKind::ProfilerExitFrameTail: {
      TrampolinePtr ptr =
          cx->runtime()->jitRuntime()->getProfilerExitFrameTail();
      return (uintptr_t)(ptr.value);
    }
    case AOTRelocKind::DoubleToInt32Stub: {
      TrampolinePtr ptr =
          cx->runtime()->jitRuntime()->getDoubleToInt32ValueStub();
      return (uintptr_t)(ptr.value);
    }
    case AOTRelocKind::MegamorphicCache:
      return (uintptr_t)&cx->runtime()->caches().megamorphicCache;
    case AOTRelocKind::MegamorphicSetPropCache:
      return (uintptr_t)cx->runtime()->caches().megamorphicSetPropCache.get();
    case AOTRelocKind::StringToAtomCache: {
      auto* cache =
          reinterpret_cast<const uint8_t*>(
              &cx->runtime()->caches().stringToAtomCache);
      return (uintptr_t)(cache + StringToAtomCache::offsetOfLastLookups());
    }
    case AOTRelocKind::DispatchTable:
    case AOTRelocKind::VMWrapper:
    case AOTRelocKind::DebugTrapHandler:
    case AOTRelocKind::CppFunction:
      MOZ_CRASH("Patch-only AOTRelocKind cannot be resolved at runtime");
    case AOTRelocKind::Count:
      break;
  }
  MOZ_CRASH("Unknown AOTRelocKind");
}


}  // namespace js::jit
