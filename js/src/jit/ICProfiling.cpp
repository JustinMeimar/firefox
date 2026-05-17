/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ICProfiling.h"

#include "gc/Zone.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIRCompiler.h"
#include "jit/ICState.h"
#include "jit/JitScript.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"

#include "gc/GC-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

bool jit::HarvestSelfHostedICProfiles(JSContext* cx, ICProfileMap* out) {
  uint32_t total = 0;

  for (auto iter = cx->zone()->cellIterUnsafe<BaseScript>(); !iter.done();
       iter.next()) {
    BaseScript* base = iter.get();
    if (!base->hasBytecode()) continue;
    JSScript* script = base->asJSScript();
    if (!script->selfHosted() || !script->hasJitScript()) continue;

    ICScript* icScript = script->jitScript()->icScript();
    uint32_t n = icScript->numICEntries();
    if (n == 0) continue;

    MonomorphicICHints hints;
    for (uint32_t i = 0; i < n; i++) {
      ICFallbackStub* fb = icScript->fallbackStub(i);
      if (fb->state().mode() != ICState::Mode::Specialized) continue;
      if (fb->numOptimizedStubs() != 1) continue;

      ICStub* stub = icScript->icEntry(i).firstStub();
      if (stub->isFallback()) continue;

      const CacheIRStubInfo* info = stub->toCacheIRStub()->stubInfo();
      CacheIRStubKey::Lookup lookup(info->kind(), ICStubEngine::Baseline,
                                    info->code(), info->codeLength());

      MonomorphicICHint hint = {i, uint32_t(CacheIRStubKey::hash(lookup)),
                               info->kind()};
      if (!hints.append(hint)) {
        return false;
      }
    }

    if (hints.empty()) continue;

    uint32_t key = script->length();
    auto ptr = out->lookupForAdd(key);
    if (ptr) continue;

    JitSpew(JitSpew_BaselineAOT,
            "Harvested %zu monomorphic ICs for self-hosted (key=%u)",
            hints.length(), key);

    if (!out->add(ptr, key, std::move(hints))) return false;
    total++;
  }

  JitSpew(JitSpew_BaselineAOT,
          "IC harvest: %u self-hosted functions with profiles", total);
  return true;
}
