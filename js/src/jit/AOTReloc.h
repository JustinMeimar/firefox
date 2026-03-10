/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTReloc_h
#define jit_AOTReloc_h

#include <cstdint>

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

// Enumerates runtime-dependent references that the JIT codegen may need.
// Each kind maps to a specific absolute address that varies
// between processes (due to ASLR, different builds, etc.).
//
// In non-AOT mode these resolve to ImmPtr at compile time. In AOT mode
// they emit movWithPatch(sentinel) and register a RuntimePatch that is
// applied at load time.

#define AOT_RELOC_KINDS(V)   \
  V(JSRuntimePtr)               \
  V(JSContextPtr)               \
  V(InterruptBits)              \
  V(JitActivation)              \
  V(RealmPtr)                   \
  V(ContextRealm)               \
  V(WellKnownSymbols)           \
  V(JitRuntime)                 \
  V(LastBufferedCell)           \
  V(ProfilerEnabled)            \
  V(ProfilerExitFrameTail)      \
  V(DoubleToInt32Stub)          \
  V(MegamorphicCache)           \
  V(MegamorphicSetPropCache)    \
  V(StringToAtomCache)          \
  V(DispatchTable)              \
  V(VMWrapper)                  \
  V(DebugTrapHandler)           \
  V(CppFunction)                \
  V(PreBarrier)                 \
  V(ExceptionTail)

enum class AOTRelocKind : uint16_t {
#define EMIT_KIND(name) name,
  AOT_RELOC_KINDS(EMIT_KIND)
#undef EMIT_KIND
  Count
};

// Map an AOTRelocKind to its string name. Usable in debug logging.
inline const char* AOTRelocKindName(AOTRelocKind kind) {
  switch (kind) {
#define EMIT_CASE(name) case AOTRelocKind::name: return #name;
    AOT_RELOC_KINDS(EMIT_CASE)
#undef EMIT_CASE
    case AOTRelocKind::Count:
      break;
  }
  return "Unknown";
}

// Resolve an AOTRelocKind to its runtime value. Used by the non-AOT path
// to get the compile-time ImmPtr value, and by the patch-based AOT path to
// verify patch correctness.
uintptr_t ResolveAOTReloc(AOTRelocKind kind, JSContext* cx);

}  // namespace js::jit

#endif  // jit_AOTReloc_h
