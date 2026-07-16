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

// Look up an AOT BaselineFunction blob whose identity hash matches
// `script`. Handles both self-hosted delazified scripts and guest
// scripts uniformly. On hit, installs the blob's baseline code on the
// script.
[[nodiscard]] bool LoadAOTBaselineFunction(JSContext* cx,
                                           HandleScript script);

// O(1) probe key: the container's BaselineFunction blob nameHash. Two
// scripts sharing a SharedImmutableScriptData collide here, which is
// fine -- the SHA-1 identity memcmp inside LoadAOTBaselineFunction is
// the ground-truth verify.
uint32_t ComputeBaselineProbeHash(JSScript* script);

[[nodiscard]] bool RecordAOTBaselineFunction(JSContext* cx,
                                             HandleScript script);

// Observation-driven IC stub recorder. Called from
// AttachBaselineCacheIRStubLocked once a CacheIR stub has been compiled
// or looked up. Dedup by SHA-1(cacheIR bytes + fieldTypes) so folded /
// re-attached stubs collapse to a single blob.
[[nodiscard]] bool RecordAOTICStub(JSContext* cx, JitCode* code,
                                   CacheIRStubInfo* stubInfo);

// Emit a `baseline-compile` line to the AOTInstr_Baseline channel when
// enabled. Fires per successful baseline compile regardless of corpus
// mode, so `JS_AOT_INSTR=baseline` alone yields per-workload frequency
// logs suitable for the fossil baseline-frequency analyses. Cheap
// no-op when the channel is off.
void EmitBaselineCompileEvent(JSContext* cx, JSScript* script);

// Ensure the runtime has an AOT preamble for `code`. Idempotent: on repeat
// calls for the same code, this is a no-op. Required so
// JSScript::updateJitCodeRaw routes callers through a trampoline that
// loads AOTSelfHostedPassReg with the runtime's indirection-table base
// before entering the realm-independent baseline body.
[[nodiscard]] bool EnsureAOTPreambleFor(JSContext* cx, JitCode* code);

[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

[[nodiscard]] bool LoadAOTICStubs(JSContext* cx);

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
