/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Instr_h
#define jit_Instr_h

#include "mozilla/Likely.h"
#include "mozilla/Span.h"

#include <cstddef>
#include <cstdint>

#include "jit/InstrIds.h"
#include "js/TypeDecls.h"

// [SMDOC] Phase-3 JIT instrumentation
//
// This subsystem produces the durable JSONL logs consumed by the
// research harness described in
// `notes/07_26_2026_instr_plan.md`. It has three design goals:
//
//   1. The log is the interface. Every event is a JSON object on one
//      line, prefixed with a versioned header, so the harness can
//      parse producer output without any coupling to engine
//      internals. Schema version is `v` on every line; bump on any
//      incompatible change.
//
//   2. Each responsibility is a small class, not a macro pile. The
//      global `JSInstr` facade is the only symbol callers touch; it
//      forwards to a private JsonlSink, a per-process InstrRegistry
//      that hands out monotonic local ids, and per-event serializers.
//      Callers never see FILE*, never see JSONWriter, never take
//      locks.
//
//   3. Every event site is O(cache-line) on the disabled path.
//      `JSInstr::Enabled(channel)` is a single relaxed atomic load;
//      when disabled, no allocation, no formatting, no lock.
//
// Runtime activation is via environment variables, read once at first
// JitRuntime init and immutable thereafter:
//
//   JS_INSTR       "1" or "all" enables every channel. Otherwise a
//                  comma-separated subset of channel names:
//                  {lifecycle, ic, demand, coupling, snapshot, timing,
//                   baseline}. Unset leaves the subsystem disabled.
//
//   JS_INSTR_DIR   Required when JS_INSTR is set. Output directory;
//                  one file per process: `<proc>.<pid>.jsonl`. If the
//                  directory cannot be opened, the subsystem stays
//                  disabled (never falls back to stderr; mixed
//                  streams break the harness).
//
//   JS_INSTR_RUN_ID  Opaque string recorded in the run-header event;
//                    lets the harness correlate multi-process output.
//                    Required when JS_INSTR is set.
//
//   JS_INSTR_MODE  "structural" or "demand". Recorded in the header.
//                  In demand mode, the baseline JIT prologue emits an
//                  additional counter increment for the per-JitScript
//                  entry counter (structural runs skip it so measured
//                  code bytes are not contaminated).

namespace js::jit {
class ExecutablePool;
class JitCode;
}  // namespace js::jit

namespace js::jit {

enum InstrChannel : uint32_t {
  InstrCh_Lifecycle = 1 << 0,  // pool/jitcode/script create+retire
  InstrCh_IC = 1 << 1,         // ic body emit, attach, detach
  InstrCh_Demand = 1 << 2,     // per-JitScript entry counter flushes
  InstrCh_Coupling = 1 << 3,   // coupling census on IC bodies
  InstrCh_Snapshot = 1 << 4,   // snapshot-marker, footprint, smaps
  InstrCh_Timing = 1 << 5,     // component timing (compile, gc, etc.)
  InstrCh_Baseline = 1 << 6,   // legacy channel: baseline compiles
  InstrCh_All = 0xFFFFFFFFu,
};

enum class InstrMode : uint8_t {
  Structural,
  Demand,
};

enum class SourceClass : uint8_t {
  SelfHosted,
  Chrome,
  Guest,
};

enum class IcEngine : uint8_t {
  Baseline,
  Ion,
};

enum class IcDetachReason : uint8_t {
  Transition,
  Fold,
  Overflow,
  TrialInline,
  IonTransition,
  GcPurge,
  WeakSweep,
  ScriptDestroy,
  RuntimeShutdown,
  Clone,
  WarpAbort,
};

enum class JitCodeOwner : uint8_t {
  BaselineScript,
  BaselineInterpreter,
  BaselineIC,
  SharedIC,
  Trampoline,
  Ion,
  Regexp,
  Wasm,
  Other,
};
inline constexpr size_t kJitCodeOwnerCount = size_t(JitCodeOwner::Other) + 1;

enum class ExecPoolKind : uint8_t {
  Baseline,
  Ion,
  Other,
  Regexp,
  Wasm,
};

const char* Name(InstrMode);
const char* Name(SourceClass);
const char* Name(IcEngine);
const char* Name(IcDetachReason);
const char* Name(JitCodeOwner);
const char* Name(ExecPoolKind);

struct CouplingRecord {
  const char* operandKind;  // e.g. "ImmGCPtr", "AbsoluteAddress"
  uint32_t patchOffset;     // offset within the code body
  const char* targetKind;   // e.g. "Shape", "Realm", "Runtime"
  const char* relocKind;    // e.g. "gcptr", "cellptr", "none"
  const char* eligibility;  // "direct-relocatable" | "table" | "instance"
};

// Facade. All engine code calls into this. The methods are declared
// as inline no-ops on the disabled path so the caller pays for a
// single atomic-relaxed load before deciding to build any arguments.
class JSInstr {
 public:
  // Process-idempotent. Called from JitRuntime::init. A second call
  // (second JitRuntime in the same PID) does not truncate the log,
  // does not reopen the file, and emits a runtime-init event instead
  // of a new run-header.
  static void Init();

  // Called from JitRuntime::finish on graceful shutdown to flush
  // buffers and emit runtime-shutdown for that runtime.
  static void RuntimeShutdown(JSRuntime* rt);

  // Called at process exit if the caller has a hook. Optional; log
  // is durable line-by-line so a hard exit is also safe.
  static void ProcessShutdown();

  // Cheap disabled-path check. The channel mask is a single relaxed
  // atomic uint32_t; the branch is predicted-not-taken.
  static bool Enabled(uint32_t channel);

  static uint32_t RuntimeLocalId(JSRuntime* rt);
  static uint32_t ScriptLocalId(JSScript* script);
  static uint32_t SiteLocalId(JSScript* script, uint32_t bcOffset);
  static bool MarkRuntimeEntriesFlushed(JSRuntime* rt);
  static bool RuntimeEntriesFlushed(JSRuntime* rt);

  // Lifecycle
  static void LogPoolCreate(ExecutablePool* pool, ExecPoolKind kind,
                            size_t mmapBytes);
  static void LogPoolUnmap(ExecutablePool* pool);

  static void LogJitCodeCreate(JitCode* code, JitCodeOwner owner);
  static void LogJitCodeFinalize(JitCode* code);

  static void LogScriptCreate(JSScript* script);
  static void LogScriptDestroy(JSScript* script);

  // Baseline compile / retire.
  //
  // semanticId is computed by BaselineInstr::ComputeSemanticId, which
  // hashes the canonical bytecode representation. codeId is over the
  // finished machine code.
  static void LogBaselineCompile(JSRuntime* rt, JSScript* script,
                                 const Sha1Digest& semanticId,
                                 const Sha1Digest& codeId, uint32_t methodBytes,
                                 uint32_t metadataBytes, uint32_t numIcEntries);
  static void LogBaselineRetire(JSScript* script);
  static void LogBaselineEntriesRetire(JSRuntime* rt, JSScript* script,
                                       uint64_t enteredCount);

  // A brand new baseline CacheIR body was compiled. Fires exactly
  // once per unique source_sha per process. `sourceSha` is the SHA
  // over the CacheIR bytecode (also serialized as `ic_body_id` for
  // correlation with attach/detach events). `codeSha` is the SHA
  // over the finished machine code. `sourceBytes` is the CacheIR
  // bytecode length; `machineBytes` is the compiled JitCode length.
  // `coupling` is a slice copied under the sink mutex.
  static void LogIcBodyEmit(const Sha1Digest& sourceSha,
                            const Sha1Digest& codeSha, const char* cacheKind,
                            uint32_t sourceBytes, uint32_t machineBytes,
                            uint32_t stubDataBytes,
                            mozilla::Span<const CouplingRecord> coupling);

  // Regexp JIT code was compiled. Fires from
  // SMRegExpMacroAssembler::GetCode after all backpatches are
  // applied. `sourceSha` is SHA over the pattern atom bytes + flags
  // (an IR-level identity); `codeSha` is SHA over the finished
  // machine code.
  static void LogRegExpEmit(const uint8_t* patternBytes, uint32_t patternLen,
                            bool patternLatin1, uint32_t flagsRaw,
                            uint32_t machineBytes, const Sha1Digest& codeSha);

  static void LogIcInstanceAttach(JSScript* outerScript, uint32_t bcOffset,
                                  const Sha1Digest& icBodyId, IcEngine engine);

  static void LogIcInstanceDetach(JSScript* outerScript, uint32_t bcOffset,
                                  const Sha1Digest& icBodyId,
                                  IcDetachReason reason, uint32_t enteredCount,
                                  bool isFallback, uint32_t chainLengthBefore);

  // Snapshot -- called from InstrSnapshot on each process. All five
  // snapshot event kinds share the same conceptual checkpoint: the
  // marker line anchors the checkpoint, and everything else is
  // per-artifact-class detail joinable by (pid, marker).
  static void LogSnapshotMarker(const char* marker);

  struct PoolInfo {
    uint32_t poolId;
    const char* poolKind;
    void* base;
    size_t mmapBytes;
    size_t usedBytes;
  };
  // Snapshot helper: hands each currently-registered ExecutablePool to
  // `cb`, called under the registry lock. Caller must not re-enter
  // JSInstr from inside `cb`.
  using PoolCallback = void (*)(void* userdata, const PoolInfo&);
  static void ForEachLivePool(void* userdata, PoolCallback cb);

  static void LogSnapshotFootprint(uint32_t poolId, const char* poolKind,
                                   size_t mmapBytes, size_t usedBytes,
                                   size_t unusedBytes);

  struct LiveByOwnerRow {
    JitCodeOwner owner;
    uint64_t count;
    uint64_t codeBytes;
  };
  struct LiveCounters {
    LiveByOwnerRow perOwner[kJitCodeOwnerCount];
    uint64_t livePoolCount;
    uint64_t liveMmapBytes;
    uint64_t liveIcBodyCount;
    uint64_t liveIcBodyBytes;
  };
  static void GetLiveCounters(LiveCounters* out);
  static void LogSnapshotLive(const LiveCounters& c);

  struct SmapsRow {
    uint64_t startAddr;
    uint64_t endAddr;
    uint64_t sizeKb;
    uint64_t rssKb;
    uint64_t pssKb;
    uint64_t sharedCleanKb;
    uint64_t sharedDirtyKb;
    uint64_t privateCleanKb;
    uint64_t privateDirtyKb;
    uint64_t referencedKb;
    uint64_t anonymousKb;
    const char* perms;
    const char* path;
  };
  static void LogSnapshotSmapsRow(const SmapsRow& r);

  // Demand mode -- flushed on shutdown, GC-purge boundaries, and
  // every snapshot. Two flat spans are joined by (icEntryStart,
  // icEntryCount): the harness walks `scripts` and for each row takes
  // a `icEntryCount`-long slice out of `icEntries` starting at
  // `icEntryStart`.
  struct IcEntryRow {
    uint32_t siteLocalId;
    Sha1Digest icBodyId;
    uint64_t enteredCount;
    bool isFallback;
  };
  struct EntriesFlushRow {
    uint32_t scriptLocalId;
    uint64_t enteredCount;
    uint32_t icEntryStart;
    uint32_t icEntryCount;
  };
  static void LogEntriesFlush(uint32_t runtimeLocalId, const char* reason,
                              mozilla::Span<const EntriesFlushRow> scripts,
                              mozilla::Span<const IcEntryRow> icEntries);
  static void LogEntriesOverflow(uint32_t scriptLocalId);
};

// Backwards-compatible convenience macro; new code should prefer the
// typed log methods. Kept for the two existing sites that used
// JS_INSTR(...) formatted output.
#define JS_INSTR_ENABLED(ch) MOZ_UNLIKELY(::js::jit::JSInstr::Enabled(ch))

}  // namespace js::jit

#endif  // jit_Instr_h
