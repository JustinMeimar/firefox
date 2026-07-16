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
#include "threading/ConditionVariable.h"
#include "threading/Mutex.h"
#include "threading/Thread.h"
#include "vm/MutexIDs.h"

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

// Emit a `baseline-compile` line to the AOTInstr_Baseline channel when
// enabled. Fires per successful baseline compile regardless of corpus
// mode, so `JS_AOT_INSTR=baseline` alone yields per-workload frequency
// logs suitable for the fossil baseline-frequency analyses. Cheap
// no-op when the channel is off.
void EmitBaselineCompileEvent(JSContext* cx, JSScript* script);

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

// Dump one baseline function blob to $JS_AOT_PGO_DIR when the baseline
// PGO channel is on. No-op when the channel is off.
void MaybeDumpBaselineFunctionForPGO(uint32_t canonicalHash,
                                     const AOTBlobWriter& blob);

// Content-addressed IC record entry point. Hashes the CacheIR body,
// dedups against JitRuntime::aotDump_.recordedICStubHashes, and hands
// the resulting blob to the AOTCorpusFlusher for background write to
// $JS_AOT_ICS_CORPUS_DIR. Only called when JitOptions.recordAOTICs is
// true. Non-fatal on any failure (matches the "record never crashes"
// contract).
void RecordAOTICStub(JSContext* cx, CacheKind kind,
                     const CacheIRWriter& writer);

// Wait for the background flusher to finish any queued writes, then
// synchronously write any residual pending blobs. Idempotent. Called
// from JS_ShutDown and from DumpAOTContainer before merge-from-disk.
void DrainPendingAOTCorpus(JSContext* cx);

// Owns a single background writer thread. Enqueue is O(1) under a
// private mutex; the writer thread wakes on a condvar, snapshots the
// queues, and writes files without holding any lock the compile thread
// contends for. Lazily started by RecordAOTBaselineFunction /
// RecordAOTICStub on first miss.
class AOTCorpusFlusher {
 public:
  struct BaselineEntry {
    uint32_t canonicalHash;
    AOTBlobWriter blob;
  };

  AOTCorpusFlusher(std::string baselineDir, std::string icDir);
  ~AOTCorpusFlusher();

  [[nodiscard]] bool ensureThreadStarted();
  void enqueueBaseline(BaselineEntry&& entry);
  void enqueueIC(AOTBlobWriter&& blob);
  void drainAndStop();

 private:
  static void ThreadEntry(AOTCorpusFlusher* self);
  void writerMain();
  void writeBatch(Vector<BaselineEntry, 0, SystemAllocPolicy>& baselineBatch,
                  Vector<AOTBlobWriter, 0, SystemAllocPolicy>& icBatch);

  std::string baselineDir_;
  std::string icDir_;

  Mutex mutex_ MOZ_UNANNOTATED{mutexid::AOTCorpusFlusher};
  ConditionVariable cv_;
  Vector<BaselineEntry, 0, SystemAllocPolicy> baselinePending_;
  Vector<AOTBlobWriter, 0, SystemAllocPolicy> icPending_;
  bool stopRequested_ = false;
  bool threadStarted_ = false;
  bool drained_ = false;
  Thread thread_;
};

#endif  // ENABLE_JS_AOT

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
