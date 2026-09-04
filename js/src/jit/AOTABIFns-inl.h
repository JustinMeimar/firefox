/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTABIFns_inl_h
#define jit_AOTABIFns_inl_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/HashFunctions.h"

#  include <stdint.h>

#  include "jit/ABIFunctionList.h"
#  include "jit/AOT.h"

namespace js::jit {

// Companion to the named slot hash, covering the ABI function half of the
// numbering. Link sites name an ABI function by index, so reordering or
// renaming an entry has to invalidate an image recorded against the old order.
constexpr mozilla::HashNumber AOTABIFnTableHash() {
  const char* const entries[] = {
#  define AOT_ABIFN(fp) #fp,
#  define AOT_ABIFN_NOLINK(fp) "!" #fp,
#  define AOT_ABIFN_TYPED(fp, ...) #fp "->" #__VA_ARGS__,
#  include "jit/AOTABIFns.tbl"
#  undef AOT_ABIFN_TYPED
#  undef AOT_ABIFN_NOLINK
#  undef AOT_ABIFN
  };
  mozilla::HashNumber h = 0;
  for (const char* e : entries) {
    h = mozilla::AddToHash(h, mozilla::HashStringUntilZero(e));
  }
  return mozilla::AddToHash(h, kAOTABIFnCount);
}

// Identifies everything an image's link sites bind against.
constexpr mozilla::HashNumber AOTImageLinkHash() {
  return mozilla::AddToHash(AOTSlotTableHash(), AOTABIFnTableHash());
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTABIFns_inl_h
