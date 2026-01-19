/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"
#include "mozilla/Assertions.h"

namespace js::jit {

static PatchResolverFn s_PatchHandlers[256] = { nullptr };

void PatchRegistry::Register(PatchHandlerID id, PatchResolverFn fn) {
    MOZ_ASSERT(id < 256);
    s_PatchHandlers[id] = fn;
}

uintptr_t PatchRegistry::Resolve(PatchHandlerID id, const PatchContext& ctx, uintptr_t payload) {
    MOZ_ASSERT(s_PatchHandlers[id]);
    return s_PatchHandlers[id](ctx, payload);
}

void InitBaselinePatches() {
    PatchRegistry::Register(DispatchTablePatch::ID, DispatchTablePatch::Resolve);
}

}  // namespace js::jit
