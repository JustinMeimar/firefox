/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include "mozilla/Span.h"

#include <string>

#include "jit/AOT.h"
#include "jit/AOTBlobGenerated.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIR.h"
#include "js/Vector.h"

namespace js::jit {

static constexpr const char* kAOTOutputPath =
    "js/src/jit/AOTBaseline.S";

// All AOT blob POD types (fields, payload) and their Encode/Decode
// helpers live in AOTBlobGenerated.h (generated from AOTBlobSchema.yaml).

[[nodiscard]] bool BuildAndSaveInterpBlob(
    JSContext* cx, const AOTPayload_BaselineInterpreter& payload);

[[nodiscard]] bool LoadAOTInterpFromContainer(
    JSContext* cx, BaselineInterpreter& interpreter);

// Look up an AOT BaselineFunction blob whose canonical bytes match
// `script`. Handles both self-hosted delazified scripts and guest
// scripts uniformly. On hit, installs the blob's baseline code on the
// script.
[[nodiscard]] bool LoadAOTBaselineFunction(JSContext* cx,
                                           HandleScript script);

// O(1) probe key: the container's BaselineFunction blob nameHash. Two
// scripts sharing a SharedImmutableScriptData collide here, which is
// fine -- the canonical memcmp inside LoadAOTBaselineFunction is the
// ground-truth verify.
uint32_t ComputeBaselineProbeHash(JSScript* script);

[[nodiscard]] bool RecordAOTBaselineFunction(JSContext* cx,
                                             HandleScript script);

// Cheap dedup check: does the accumulator already contain a baseline
// blob for this script's canonical hash? Used by the guest baseline
// corpus path to avoid re-AOT-compiling the same script every warmup.
bool IsAOTBaselineFunctionRecorded(JSContext* cx, JSScript* script);

// Ensure the runtime has an AOT preamble for `code`. Idempotent: on repeat
// calls for the same code, this is a no-op. Required so
// JSScript::updateJitCodeRaw routes callers through a trampoline that
// loads AOTSelfHostedPassReg with the runtime's indirection-table base
// before entering the realm-independent baseline body.
[[nodiscard]] bool EnsureAOTPreambleFor(JSContext* cx, JitCode* code);

[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

[[nodiscard]] bool LoadAOTICStubs(JSContext* cx);

class CacheIRWriter;

#ifdef ENABLE_JS_AOT

// Write one IC stub's CacheIR body to <dir>/IC-<hash>. Filename is a
// content hash so re-runs are idempotent. Silently no-ops on fopen failure.
void DumpAOTICStubToDir(const char* dir, CacheKind kind,
                        const CacheIRWriter& writer);

// Dump one IC stub's CacheIR body to $JS_AOT_PGO_DIR/IC-<hash> so
// SelectAOTCorpus.py can knapsack-pick the corpus from a PGO run.
// No-op if IC instrumentation is disabled or this is an AOT-fill compile.
void MaybeDumpICStubForPGO(CacheKind kind, const CacheIRWriter& writer,
                            bool isAOTFill);

// Write one baseline function blob to <dir>/BL-<canonicalHash>.bin.
// Filename is content-addressed so re-runs are idempotent. Silently
// no-ops on fopen failure, matching DumpAOTICStubToDir.
void DumpAOTBaselineFunctionToDir(const char* dir, uint32_t canonicalHash,
                                  const AOTBlobWriter& blob);

// Dump one baseline function blob to the corpus dir when
// --aot-baseline-corpus-enforce is set (target dir defaults to
// js/src/baselines, overridable with JS_AOT_BASELINE_CORPUS_DIR), and to
// $JS_AOT_PGO_DIR when the baseline PGO channel is on. Both branches
// are independent no-ops when their respective triggers are unset.
void MaybeDumpBaselineFunctionForPGO(uint32_t canonicalHash,
                                     const AOTBlobWriter& blob);

#endif  // ENABLE_JS_AOT

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
