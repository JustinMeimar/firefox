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

// All AOT blob POD types (manifest, payload) and their Encode/Decode
// helpers live in AOTBlobGenerated.h (generated from AOTBlobSchema.yaml).

[[nodiscard]] bool BuildAndSaveInterpBlob(
    JitCode* code, const AOTPayload_BaselineInterpreter& payload);

[[nodiscard]] bool LoadAOTInterpFromContainer(
    JSContext* cx, BaselineInterpreter& interpreter);

// Look up an AOT BaselineFunction blob whose canonical bytes match
// `script`. Handles both self-hosted delazified scripts and guest
// scripts uniformly. On hit, installs the blob's baseline code on the
// script.
[[nodiscard]] bool LoadAOTBaselineFunction(JSContext* cx,
                                           HandleScript script);

[[nodiscard]] bool RecordAOTBaselineFunction(JSContext* cx,
                                             HandleScript script);

[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

[[nodiscard]] bool LoadAOTICStubs(JSContext* cx);

Vector<AOTBlobWriter, 0, SystemAllocPolicy> TakeSavedICBlobs();

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

#endif  // ENABLE_JS_AOT

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
