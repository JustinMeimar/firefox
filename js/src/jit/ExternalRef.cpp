/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ExternalRef.h"

#include "jit/JitRuntime.h"
#include "vm/Caches.h"
#include "vm/JSContext.h"

namespace js::jit {

uintptr_t ResolveExternalRef(ExternalRefKind kind, JSContext* cx) {
  switch (kind) {
    case ExternalRefKind::JSContextPtr:
      return (uintptr_t)cx;
    case ExternalRefKind::InterruptBits:
      return (uintptr_t)cx->addressOfInterruptBits();
    case ExternalRefKind::JitActivation:
      return (uintptr_t)cx->addressOfJitActivation();
    case ExternalRefKind::RealmPtr:
      return (uintptr_t)cx->addressOfRealm();
    case ExternalRefKind::ContextRealm:
      return (uintptr_t)(reinterpret_cast<const uint8_t*>(cx) +
                         JSContext::offsetOfRealm());
    case ExternalRefKind::WellKnownSymbols:
      return (uintptr_t)cx->runtime()->wellKnownSymbols.ref();
    case ExternalRefKind::JitRuntime:
      return (uintptr_t)cx->runtime()->jitRuntime();
    case ExternalRefKind::LastBufferedCell:
      return (uintptr_t)cx->runtime()->gc.addressOfLastBufferedWholeCell();
    case ExternalRefKind::ProfilerEnabled:
      return (uintptr_t)cx->runtime()->geckoProfiler().addressOfEnabled();
    case ExternalRefKind::ProfilerExitFrameTail: {
      TrampolinePtr ptr =
          cx->runtime()->jitRuntime()->getProfilerExitFrameTail();
      return (uintptr_t)(ptr.value);
    }
    case ExternalRefKind::DoubleToInt32Stub: {
      TrampolinePtr ptr =
          cx->runtime()->jitRuntime()->getDoubleToInt32ValueStub();
      return (uintptr_t)(ptr.value);
    }
    case ExternalRefKind::MegamorphicCache:
      return (uintptr_t)&cx->runtime()->caches().megamorphicCache;
    case ExternalRefKind::MegamorphicSetPropCache:
      return (uintptr_t)cx->runtime()->caches().megamorphicSetPropCache.get();
    case ExternalRefKind::StringToAtomCache: {
      auto* cache =
          reinterpret_cast<const uint8_t*>(
              &cx->runtime()->caches().stringToAtomCache);
      return (uintptr_t)(cache + StringToAtomCache::offsetOfLastLookups());
    }
    case ExternalRefKind::Count:
      break;
  }
  MOZ_CRASH("Unknown ExternalRefKind");
}

RuntimePatch::Kind ExternalRefKindToPatchKind(ExternalRefKind kind) {
  switch (kind) {
    case ExternalRefKind::JSContextPtr:
      return RuntimePatch::Kind::JSContextPtr;
    case ExternalRefKind::InterruptBits:
      return RuntimePatch::Kind::InterruptBits;
    case ExternalRefKind::JitActivation:
      return RuntimePatch::Kind::JitActivation;
    case ExternalRefKind::RealmPtr:
      return RuntimePatch::Kind::RealmPtr;
    case ExternalRefKind::ContextRealm:
      return RuntimePatch::Kind::ContextRealm;
    case ExternalRefKind::WellKnownSymbols:
      return RuntimePatch::Kind::WellKnownSymbols;
    case ExternalRefKind::JitRuntime:
      return RuntimePatch::Kind::JitRuntime;
    case ExternalRefKind::LastBufferedCell:
      return RuntimePatch::Kind::LastBufferedCell;
    case ExternalRefKind::ProfilerEnabled:
      return RuntimePatch::Kind::ProfilerEnabled;
    case ExternalRefKind::ProfilerExitFrameTail:
      return RuntimePatch::Kind::ProfilerExitFrameTail;
    case ExternalRefKind::DoubleToInt32Stub:
      return RuntimePatch::Kind::DoubleToInt32Stub;
    case ExternalRefKind::MegamorphicCache:
    case ExternalRefKind::MegamorphicSetPropCache:
    case ExternalRefKind::StringToAtomCache:
      // These are only used by the register-indirect (IC) strategy
      // and have no corresponding RuntimePatch::Kind.
      MOZ_CRASH("No patch kind for IC-only ExternalRefKind");
    case ExternalRefKind::Count:
      break;
  }
  MOZ_CRASH("Unknown ExternalRefKind");
}

}  // namespace js::jit
