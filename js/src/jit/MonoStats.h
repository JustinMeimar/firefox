/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MonoStats_h
#define jit_MonoStats_h

#include "mozilla/Span.h"

#include <stddef.h>
#include <stdint.h>

class JSScript;

namespace js {
namespace jit {

class BaselineScript;
class ICCacheIRStub;
class ICFallbackStub;
class ICScript;
class JitCode;
enum class CacheOp : uint16_t;

// Diagnostics of Baseline code and monomorphic Baseline code, for release
// builds and the browser. Each process writes its own files:
//
// JS_MONO_STATS=DIR writes DIR/mono-PID.events, one JSON object per line:
//   init: the JSOp, CacheKind and CacheOp tables the other events index.
//   compile: a Baseline compile, with why the script was or wasn't compiled
//     monomorphic, its code bytes, its bytecode and, per IC op, its IC state,
//     how its IC was emitted and, as hex JSValueTypes, the types its stack
//     inputs were known to have.
//   exit: monomorphic code continued in the Baseline Interpreter.
//   invalidate: the stubs of monomorphic code were discarded.
//   discard, destroy: failed monomorphic code was discarded, Baseline code was
//     freed.
//   attach: an IC stub was attached or folded into a folded stub.
// Stub CacheIR is written as dot-separated ops, each its CacheOp index and,
// after a colon, its argument bytes in hex.
// JS_MONO_COUNTS=1, with JS_MONO_STATS, also has Baseline code count its
//   entries, the runs of each of its nic IC entries and the entries of each
//   basic block starting at a pc offset of the blocks field of its compile
//   event in DIR/mono-PID.counters, an array of uint64_t indexed from the ctr
//   field of compile events. Blocks start at jump targets and after jumps.
//   Loop heads count OSR entries; generator resumes are not counted.
// JS_MONO_CODEMAP=DIR writes DIR/codemap-PID.txt, a line per JIT code range:
//   "ns start bytes kind name", where later lines win over earlier ones.

// Why a script was or wasn't compiled monomorphic.
enum class MonoScriptReason : uint8_t {
  Mono,
  OptionOff,
  OffThread,
  Debug,
  Profiler,
  Failed,
  RealmIndependent,
  Generator,
  Async,
  NotMonomorphic,
};

// How the IC of an op was emitted.
enum class MonoICDisposition : uint8_t {
  // The IC chain is called.
  Chain,
  // Monomorphic code, but the IC has no stub, so its chain is called.
  Empty,
  // Monomorphic code, but the IC isn't monomorphic, so its chain is called.
  Polymorphic,
  Inline,
  Copy,
  // Monomorphic code, but the op pushed values before its IC.
  Pushed,
  // Monomorphic code, but the operands of the op are not supported.
  Operands,
  // Monomorphic code, but the stub couldn't be copied.
  NoCopy,
};

// Why the stub of a monomorphic IC wasn't emitted inline.
enum class MonoNoInline : uint8_t {
  None,
  GCCalls,
  NoCopy,
  Op,
};

// How an inline stub was emitted. See MonoTemplates.h.
enum class MonoTemplateUse : uint8_t {
  // Compiled, with templates off or fields no template can hold.
  None,
  // Copied from a template in the cache.
  Hit,
  // Copied from a template built for it.
  Built,
  // Compiled, since no template can be built for its key.
  Rejected,
};

struct MonoStatsIC {
  uint32_t entryIndex;
  uint32_t pcOffset;
  uint32_t start;
  uint32_t end;
  uint32_t exitBytes;
  uint32_t emitNs;
  uint32_t exitNs;
  MonoICDisposition disposition;
  MonoNoInline noInline;
  CacheOp noInlineOp;
  MonoTemplateUse templateUse;
  // The types the stack inputs of an inline stub were known to have when it
  // was emitted, as JSValueTypes.
  uint8_t numInputTypes;
  uint8_t inputTypes[3];
};

bool MonoStatsEnabled();
bool MonoCountsEnabled();
bool MonoCodeMapEnabled();

// Returns zeroed counters, or nullptr once the counters file is full or
// counting is off, with their index in the file.
uint64_t* NewMonoCounters(size_t count, int64_t* index);

uint32_t NextMonoCompileId();

int64_t MonoStatsNowNs();

void LogMonoCompile(JSScript* script, BaselineScript* baselineScript,
                    MonoScriptReason reason, int64_t countersIndex,
                    uint32_t exitsStart, uint32_t exitsEnd, int64_t codegenNs,
                    int64_t linkNs, mozilla::Span<const MonoStatsIC> ics,
                    mozilla::Span<const uint32_t> blocks);
void LogMonoExit(JSScript* script, BaselineScript* baselineScript,
                 uint32_t pcOffset);
void LogMonoScriptEvent(const char* event, JSScript* script,
                        BaselineScript* baselineScript);
void LogMonoDiscardBlocked(JSScript* script, BaselineScript* baselineScript,
                           uint32_t blocked, uint32_t monoFrames,
                           uint32_t jitFrames);
void LogMonoAttach(JSScript* script, ICScript* icScript,
                   ICFallbackStub* fallback, ICCacheIRStub* stub, bool folded);

void LogMonoCode(const void* start, size_t bytes, const char* kind,
                 const char* name);
void LogMonoBaselineCode(JSScript* script, BaselineScript* baselineScript,
                         uint32_t exitsStart, uint32_t exitsEnd,
                         mozilla::Span<const MonoStatsIC> ics);

}  // namespace jit
}  // namespace js

#endif /* jit_MonoStats_h */
