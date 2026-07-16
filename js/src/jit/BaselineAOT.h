/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include "jit/AOT.h"
#include "jit/AOTBlobGenerated.h"
#include "jit/BaselineJIT.h"

namespace js::jit {

// Default output paths for the two raw binaries consumed by
// aot_baseline/AOTBaselineIncbin.S via .incbin. Both are relative to
// the shell's CWD unless overridden by JS_AOT_TEXT_BIN /
// JS_AOT_CONTAINER_BIN environment variables (see DumpAOTContainer).
static constexpr const char* kAOTTextBinDefault = "AOTBaselineText.bin";
static constexpr const char* kAOTContainerBinDefault =
    "AOTBaselineContainer.bin";

// All AOT blob POD types (fields, payload) and their Encode/Decode
// helpers live in AOTBlobGenerated.h (generated from AOTBlobSchema.yaml).

[[nodiscard]] bool BuildAndSaveInterpBlob(
    JSContext* cx, const AOTPayload_BaselineInterpreter& payload);

[[nodiscard]] bool LoadAOTInterpFromContainer(
    JSContext* cx, BaselineInterpreter& interpreter);

[[nodiscard]] bool LoadAOTBaselineFunction(JSContext* cx,
                                           HandleScript script);

// Fast probe key. Colliding scripts are disambiguated by the identity
// hash on the load path.
uint32_t ComputeBaselineProbeHash(JSScript* script);

[[nodiscard]] bool RecordAOTBaselineFunction(JSContext* cx,
                                             HandleScript script);

// Dedup by hash of stub identity so re-attached stubs collapse.
[[nodiscard]] bool RecordAOTICStub(JSContext* cx, JitCode* code,
                                   CacheIRStubInfo* stubInfo);

// Cheap no-op when the instrumentation channel is off.
void EmitBaselineCompileEvent(JSContext* cx, JSScript* script);

// Idempotent.
[[nodiscard]] bool EnsureAOTPreambleTrampolineFor(JSContext* cx, JitCode* code);

[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

[[nodiscard]] bool LoadAOTICStubs(JSContext* cx);

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
