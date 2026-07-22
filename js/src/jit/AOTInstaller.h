/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTInstaller_h
#define jit_AOTInstaller_h

#ifdef ENABLE_JS_AOT

#  include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class BaselineInterpreter;

// [SMDOC] AOT Installer
// =====================
//
// Load side of the AOT pipeline. Given the embedded AOT image, an
// installer routine reconstructs the corresponding runtime object from
// a static text range plus its wire metadata:
//
//   installInterpreter -> BaselineInterpreter
//   installBaselineScript / installICStubs land in patch 12.
//
// All returns are recoverable: `true` means the artifact was installed
// from AOT; `false` means the caller should fall back to runtime
// codegen. The AOT loader never fatal-errors on stale / absent
// images; the shell is fully usable without an image.
[[nodiscard]] bool TryInstallAOTBaselineInterpreter(
    JSContext* cx, BaselineInterpreter& interp);

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTInstaller_h
