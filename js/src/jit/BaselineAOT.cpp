/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineAOT.h"
#include "jit/JitSpewer.h"
#include "mozilla/Assertions.h"

namespace js::jit {

void applyPatch(const PatchContext& ctx, const DispatchTablePatch& entry) {
    uint8_t* target = ctx.codeBase + ctx.dispatchTableOffset +
                      (entry.dispatchTableIndex * sizeof(uintptr_t));
    uintptr_t val = uintptr_t(ctx.codeBase + entry.handlerOffset);
    *reinterpret_cast<uintptr_t*>(target) = val;
}

void AOTBlobLayout::dump(bool isLoad, void* blobStart) const {
  if (isLoad) {
    JitSpew(JitSpew_BaselineAOT, "Loading AOT baseline blob: start=%p, size=%zu",
            blobStart, blobSize());
  }
  JitSpew(JitSpew_BaselineAOT, "Code: offset=0, size=%zu", codeSize);
  JitSpew(JitSpew_BaselineAOT, "  Prologue: offset=0, size=%zu", prologueSize());
  JitSpew(JitSpew_BaselineAOT, "  Handlers: offset=%u, size=%zu", interpretOpOffset, handlersSize());
  JitSpew(JitSpew_BaselineAOT, "  Dispatch table: offset=%u, size=%zu",
          dispatchTableOffset, dispatchTableSize());
  JitSpew(JitSpew_BaselineAOT, "Metadata: offset=%zu, size=%zu",
          manifestOffset(), metadataSize());
  JitSpew(JitSpew_BaselineAOT, "  Manifest: offset=%zu, size=%zu",
          manifestOffset(), manifestSize());
  JitSpew(JitSpew_BaselineAOT, "  Debug instr: offset=%zu, size=%zu (%u entries)",
          debugInstrOffset(), debugInstrSize(), debugInstrCount);
  JitSpew(JitSpew_BaselineAOT, "  Debug traps: offset=%zu, size=%zu (%u entries)",
          debugTrapOffset(), debugTrapSize(), debugTrapCount);
  JitSpew(JitSpew_BaselineAOT, "  Code coverage: offset=%zu, size=%zu (%u entries)",
          codeCoverageOffset(), codeCoverageSize(), codeCoverageCount);
  JitSpew(JitSpew_BaselineAOT, "  IC returns: offset=%zu, size=%zu (%u entries)",
          icReturnOffset(), icReturnSize(), icReturnCount);
  JitSpew(JitSpew_BaselineAOT, "  Patches: offset=%zu, size=%zu (%u entries)",
          patchOffset(), patchSize(), patchCount);
  JitSpew(JitSpew_BaselineAOT, "  OpHandlers: offset=%zu, size=%zu (%u entries)",
          opHandlerOffset(), opHandlerSize(), opHandlerCount);
  JitSpew(JitSpew_BaselineAOT, "  Footer: offset=%zu, size=%zu",
          footerOffset(), footerSize());
}

}  // namespace js::jit
