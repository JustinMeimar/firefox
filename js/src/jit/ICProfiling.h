/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ICProfiling_h
#define jit_ICProfiling_h

#include <stdint.h>

#include "jit/CacheIR.h"
#include "js/HashTable.h"
#include "js/Vector.h"

struct JSContext;

namespace js::jit {

class ICScript;

struct MonomorphicICHint {
  uint32_t entryIndex;
  uint32_t stubKeyHash;
  CacheKind kind;
};

using MonomorphicICHints = Vector<MonomorphicICHint, 16, SystemAllocPolicy>;

using ICProfileMap =
    HashMap<uint32_t, MonomorphicICHints, DefaultHasher<uint32_t>,
            SystemAllocPolicy>;

[[nodiscard]] bool HarvestSelfHostedICProfiles(JSContext* cx,
                                               ICProfileMap* profileMap);

}  // namespace js::jit

#endif /* jit_ICProfiling_h */
