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
// Load side of the AOT pipeline. Given the embedded AOT image, an
// installer routine reconstructs the corresponding runtime object from
// a static text range plus its wire metadata:
//
//   TryInstallAOTBaselineInterpreter -> BaselineInterpreter
//   TryInstallAOTBaselineScript      -> attaches BaselineScript to
//                                       a JSScript (patch 12)
//   TryLoadAOTICStubs                -> populates a JitZone's
//                                       baselineCacheIRStubCodes_
//                                       (patch 12)
//
// All returns are recoverable: `true` means the artifact was installed
// from AOT; `false` means the caller should fall back to runtime
// codegen. Under JitOptions.aotEnforce the caller is expected to
// treat a `false` as a hard error (crash), so the AOT contract holds
// end-to-end in CI.
[[nodiscard]] bool TryInstallAOTBaselineInterpreter(
    JSContext* cx, BaselineInterpreter& interp);

[[nodiscard]] bool TryInstallAOTBaselineScript(JSContext* cx,
                                               JS::HandleScript script);

[[nodiscard]] bool TryLoadAOTICStubs(JSContext* cx, JitZone* jitZone);

// Fast prefilter for baseline function lookups. Colliding scripts are
// disambiguated by the identity hash on the load path.
uint32_t ComputeBaselineProbeHash(JSScript* script);

// SHA-1 over the JSScript state the baseline compiler reads. Two
// scripts hash equal iff a baseline blob compiled for one is
// byte-compatible with the other. Any change to what is hashed
// invalidates existing corpora; bump image::kVersion alongside.
void ComputeBaselineIdentityHash(JSScript* script,
                                 mozilla::SHA1Sum::Hash& out);

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTInstaller_h
