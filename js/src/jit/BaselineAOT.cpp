/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"
#include "mozilla/Assertions.h"

namespace js::jit {

void applyPatch(const PatchContext& ctx, const DispatchTablePatch& entry) {
    uint8_t* target = ctx.codeBase + ctx.dispatchTableOffset +
                      (entry.dispatchTableIndex * sizeof(uintptr_t));
    uintptr_t val = uintptr_t(ctx.codeBase + entry.handlerOffset);
    *reinterpret_cast<uintptr_t*>(target) = val;
}

}  // namespace js::jit
