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

// Enumerates runtime-dependent references that the JIT codegen may need.
// Each kind maps to a specific absolute address that varies
// between processes (due to ASLR, different builds, etc.).
//
// The wrappers (movePtr, loadPtr, branch32, storePtr) which take runtime
// pointers accept ExternalRefKind and internally dispatch to the right
// strategy:
//   1) Non-AOT: resolve to ImmPtr/AbsoluteAddress at compile time
//   2) AOT PatchBasedBlob: emit sentinel via movWithPatch + register patch
//   3) AOT RegisterIndirect: emit zone->runtime->field load chain

// Only in ExternalRefKind (register-indirect strategy, no patching).
#define EXTERNAL_REF_ONLY_KINDS(V) \
  V(MegamorphicCache)          \
  V(MegamorphicSetPropCache)   \
  V(StringToAtomCache)

#define EXTERNAL_REF_KINDS(V)   \
  EXTERNAL_REF_SHARED_KINDS(V) \
  EXTERNAL_REF_ONLY_KINDS(V)

enum class ExternalRefKind : uint16_t {
#define EMIT_KIND(name) name,
  EXTERNAL_REF_KINDS(EMIT_KIND)
#undef EMIT_KIND
  Count
};

// Resolve an ExternalRefKind to its runtime value. Used by the non-AOT path
// to get the compile-time ImmPtr value, and by the patch-based AOT path to
// verify patch correctness.
uintptr_t ResolveExternalRef(ExternalRefKind kind, JSContext* cx);

// Shared kinds are emitted first in both ExternalRefKind and
// RuntimePatch::Kind so a static_cast between them is valid.
#define CHECK_SHARED_ORDINAL(name) \
  static_assert(static_cast<uint16_t>(ExternalRefKind::name) == \
                static_cast<uint16_t>(RuntimePatch::Kind::name), \
                "ExternalRefKind::" #name " != RuntimePatch::Kind::" #name);
   EXTERNAL_REF_SHARED_KINDS(CHECK_SHARED_ORDINAL)
#undef CHECK_SHARED_ORDINAL

}  // namespace js::jit

#endif  // jit_ExternalRef_h
