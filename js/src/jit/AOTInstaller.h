/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTInstaller_h
#define jit_AOTInstaller_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/SHA1.h"

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
// Installing the baseline interpreter is required and fails when its image data
// is missing or invalid. Baseline scripts and inline cache stubs may fall back
// to runtime code generation unless strict AOT enforcement is enabled.
[[nodiscard]] bool InstallAOTBaselineInterpreter(JSContext* cx,
                                                 BaselineInterpreter& interp);

[[nodiscard]] bool TryInstallAOTBaselineScript(JSContext* cx,
                                               JS::HandleScript script);

[[nodiscard]] bool TryLoadAOTICStubs(JSContext* cx, JitZone* jitZone);

// Fast prefilter for baseline function lookups. Colliding scripts are
// disambiguated by the identity hash on the load path.
uint32_t ComputeBaselineProbeHash(JSScript* script);

// The identity hash covers the script state read during baseline compilation.
// Equal hashes mean the compiled artifact is byte compatible. Changes to the
// hash inputs require a new image format version.
void ComputeBaselineIdentityHash(JSScript* script, mozilla::SHA1Sum::Hash& out);

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTInstaller_h
