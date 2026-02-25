/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ExternalRef_h
#define jit_ExternalRef_h

#include <cstdint>

#include "jit/BaselineAOT.h"

struct JSContext;

namespace js::jit {

// Enumerates all non-parametric runtime-dependent references that the JIT
// codegen may need. Each kind maps to a specific absolute address that varies
// between processes (due to ASLR, different builds, etc.).
//
// The unified MacroAssembler overloads (movePtr, loadPtr, branch32, storePtr)
// accept ExternalRefKind and internally dispatch to the right strategy:
//   - Non-AOT: resolve to ImmPtr/AbsoluteAddress at compile time
//   - AOT PatchBasedBlob: emit sentinel via movWithPatch + register patch
//   - AOT RegisterIndirect: emit zone->runtime->field load chain
enum class ExternalRefKind : uint16_t {
  // JSContext pointer (the main context).
  JSContextPtr,
  // Address of interrupt bits for interrupt checks.
  InterruptBits,
  // Address of JIT activation.
  JitActivation,
  // Address of the realm pointer in JSContext.
  RealmPtr,
  // Address of context realm (cx + offsetOfRealm).
  ContextRealm,
  // Pointer to well-known symbols table.
  WellKnownSymbols,
  // Pointer to the JitRuntime.
  JitRuntime,
  // Address of the last buffered whole cell for GC.
  LastBufferedCell,
  // Address of the profiler enabled flag.
  ProfilerEnabled,
  // Address of the profiler exit frame tail trampoline.
  ProfilerExitFrameTail,
  // Address of the double-to-int32 conversion stub.
  DoubleToInt32Stub,

  // Address of the megamorphic cache.
  MegamorphicCache,
  // Pointer to the megamorphic set-prop cache.
  MegamorphicSetPropCache,
  // Address of the string-to-atom cache (+ lastLookups offset).
  StringToAtomCache,

  Count
};

// Resolve an ExternalRefKind to its runtime value. Used by the non-AOT path
// to get the compile-time ImmPtr value, and by the patch-based AOT path to
// verify patch correctness.
uintptr_t ResolveExternalRef(ExternalRefKind kind, JSContext* cx);

// Map an ExternalRefKind to the corresponding RuntimePatch::Kind for the
// patch-based AOT strategy. This allows the unified methods to reuse the
// existing RuntimePatch infrastructure.
RuntimePatch::Kind ExternalRefKindToPatchKind(ExternalRefKind kind);

}  // namespace js::jit

#endif  // jit_ExternalRef_h
