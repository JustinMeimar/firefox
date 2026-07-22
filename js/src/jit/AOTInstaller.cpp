/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTInstaller.h"

#  include "jit/AOT.h"
#  include "jit/AOTImage.h"
#  include "jit/AOTImageGenerated.h"
#  include "jit/BaselineJIT.h"
#  include "jit/JitCode.h"
#  include "jit/JitOptions.h"
#  include "jit/JitRuntime.h"
#  include "jit/JitSpewer.h"
#  include "vm/JSContext.h"

namespace js::jit {

bool TryInstallAOTBaselineInterpreter(JSContext* cx,
                                      BaselineInterpreter& interp) {
  if (!JitOptions.useAOTImage) {
    return false;
  }

  const AOTImage* image = AOTImage::embedded();
  if (!image) {
    return false;
  }

  auto readerOpt = image->findUnique(AOTBlobKind::BaselineInterpreter);
  if (readerOpt.isNothing()) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT image lacks a baseline interpreter blob");
    return false;
  }

  AOTBlobReader reader = readerOpt.ref();
  BaselineInterpreterMetadata md;
  if (!DecodeBlob_BaselineInterpreter(reader, &md)) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT baseline interpreter decode failed");
    return false;
  }

  // The image's text region is r-x pages; JitCode::NewStatic wraps them
  // without an ExecutablePool or JitCodeHeader.
  auto code = reader.code();
  uint8_t* codeStart = const_cast<uint8_t*>(code.data());
  JitCode* jitCode =
      AllocateAOTCode(cx, codeStart, uint32_t(code.size()), CodeKind::Other);
  if (!jitCode) {
    return false;
  }

  interp.init(jitCode, std::move(md));

  if (!EnsureAOTPreambleTrampolineFor(cx, jitCode)) {
    return false;
  }

  JitSpew(JitSpew_BaselineAOT,
          "installed baseline interpreter from AOT image: bytes=%zu",
          size_t(code.size()));
  return true;
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT
