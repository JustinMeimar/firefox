/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineInstr_h
#define jit_BaselineInstr_h

#include "jit/Instr.h"
#include "jit/InstrIds.h"
#include "js/TypeDecls.h"

// Baseline-specific instrumentation helpers. The identity of a
// baseline-compiled script for the phase-3 log is a SHA-1 over its
// canonical bytecode representation: two scripts share semantic_id
// iff baseline codegen would emit byte-identical code (up to the
// codegen flags recorded on the run-header).
//
// This header only exposes computation helpers; emission is done by
// JSInstr from `BaselineJIT.cpp` at compile time.

namespace js::jit {

class BaselineScript;
class CacheIRStubInfo;
enum class CacheKind : uint8_t;
class ICEntry;
class ICFallbackStub;
class ICCacheIRStub;
class ICStub;

// SHA-1 identity as defined by [SMDOC] Phase-3 instrumentation
// identities in InstrIds.h.
Sha1Digest ComputeBaselineSemanticId(JSScript* script);

// Code identity over the finished baseline JitCode + relocation
// layout. Called immediately after BaselineScript::New completes.
Sha1Digest ComputeBaselineCodeId(BaselineScript* baseline);

// Emit a `baseline-compile` line if the Baseline channel is enabled.
// Idempotent-safe wrapper around JSInstr::LogBaselineCompile.
void EmitBaselineCompileEvent(JSContext* cx, JSScript* script);

// Walk the whole IC chain reachable from `icEntry->firstStub()`
// through `fallback` and emit one ic-instance-detach per stub,
// including the fallback itself.
//
// MUST be called BEFORE the caller invokes discardStubs / unlinkStub:
// after the unlink the chain no longer reaches the removed stubs and
// their enteredCount_ is lost.
//
// `outerScript` is the script whose ICEntry contains this chain, used
// to derive site_id. `reason` classifies why the chain is being torn
// down.
void HarvestIcChain(ICEntry* icEntry, ICFallbackStub* fallback,
                    JSScript* outerScript, IcDetachReason reason);

// Emit ic-instance-detach for a single stub in a chain (weak-sweep
// path). The rest of the chain remains intact.
void HarvestOneIcStub(ICCacheIRStub* stub, ICFallbackStub* fallback,
                      JSScript* outerScript, IcDetachReason reason);

// If this CacheIR body has not been seen before in this process, emit
// an ic-body-emit event with both source_sha (CacheIR bytecode) and
// code_sha (compiled JitCode bytes), plus a stub-data-derived coupling
// census. Called from LookupOrCompileStub's miss branch. `code` may be
// null on the portable-baseline path (no native code compiled); the
// event still fires with a zero code_sha and zero machine_bytes so
// harnesses see the CacheIR source identity.
void EmitIcBodyIfNew(CacheKind kind, const CacheIRStubInfo* stubInfo,
                     JitCode* code);

}  // namespace js::jit

#endif  // jit_BaselineInstr_h
