/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/TrampolineAOT.h"
#include "jit/JitSpewer.h"
#include "mozilla/Assertions.h"

namespace js::jit {

#ifdef ENABLE_AOT_TRAMPOLINES

void TrampolineAOTLayout::dump(bool isLoad, void* blobStart) const {
  if (isLoad) {
    JitSpew(JitSpew_BaselineAOT, "Loading AOT trampoline blob: start=%p, size=%zu",
            blobStart, blobSize());
  } else {
    JitSpew(JitSpew_BaselineAOT, "Serializing AOT trampoline blob: size=%zu",
            blobSize());
  }

  JitSpew(JitSpew_BaselineAOT, "Code: offset=0, size=%zu", codeSize);
  JitSpew(JitSpew_BaselineAOT, "Metadata: offset=%zu, size=%zu",
          manifestOffset(), metadataSize());
  JitSpew(JitSpew_BaselineAOT, "  Manifest: offset=%zu, size=%zu",
          manifestOffset(), manifestSize());
  JitSpew(JitSpew_BaselineAOT, "  VM Wrappers: offset=%zu, size=%zu (%u entries)",
          vmWrapperArrayOffset(), vmWrapperArraySize(), vmWrapperCount);
  JitSpew(JitSpew_BaselineAOT, "  Trampoline Natives: offset=%zu, size=%zu (%u entries)",
          trampolineNativeArrayOffset(), trampolineNativeArraySize(),
          trampolineNativeCount);
  JitSpew(JitSpew_BaselineAOT, "  Footer: offset=%zu, size=%zu",
          footerOffset(), footerSize());
}

#endif  // ENABLE_AOT_TRAMPOLINES

}  // namespace js::jit
