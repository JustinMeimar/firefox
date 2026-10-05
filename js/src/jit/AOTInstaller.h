/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTInstaller_h
#define jit_AOTInstaller_h

#ifdef ENABLE_JS_AOT

#  include <cstdint>

#  include "jstypes.h"

#  include "js/RootingAPI.h"
#  include "js/TypeDecls.h"

struct JS_PUBLIC_API JSContext;
class JSScript;

namespace js::jit {

class BaselineInterpreter;
class JitZone;

// [SMDOC] AOT Installer
// =====================
//
// Incompatible or absent artifacts are cache misses. Strict AOT enforcement
// is handled by callers.
[[nodiscard]] bool InstallAOTBaselineInterpreter(JSContext* cx,
                                                 BaselineInterpreter& interp);

[[nodiscard]] bool TryInstallAOTBaselineScript(JSContext* cx,
                                               JS::HandleScript script);

[[nodiscard]] bool TryLoadAOTICStubs(JSContext* cx, JitZone* jitZone);

// Fast prefilter for baseline function lookups. Colliding scripts are
// disambiguated by exact compilation inputs on the load path.
uint32_t ComputeBaselineProbeHash(JSScript* script);

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTInstaller_h
