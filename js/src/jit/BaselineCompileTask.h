/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCompileTask_h
#define jit_BaselineCompileTask_h

#include "mozilla/HashFunctions.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"

#include "jit/BaselineCodeGen.h"
#include "jit/JitScript.h"
#include "jit/OffthreadSnapshot.h"
#include "vm/HelperThreadTask.h"

namespace js::jit {

class BaselineSnapshot {
  OffthreadGCPtr<JSScript*> script_;
  OffthreadGCPtr<GlobalLexicalEnvironmentObject*> globalLexical_;
  OffthreadGCPtr<JSObject*> globalThis_;
  OffthreadGCPtr<CallObject*> callObjectTemplate_;
  OffthreadGCPtr<NamedLambdaObject*> namedLambdaTemplate_;
  size_t nargs_;
  uint32_t baseWarmUpThreshold_;
  bool isIonCompileable_;
  bool compileDebugInstrumentation_;
  MonoScriptReason monoReason_ = MonoScriptReason::OptionOff;
  // The ICs that monomorphic code is compiled from, if the script is compiled
  // monomorphic.
  MonoIC* monoICs_ = nullptr;
  size_t numMonoICs_ = 0;
  // Whether the script is compiled monomorphic before it is warm, and the hash
  // of its ICs when they were taken, which they must still have when the code
  // is used.
  bool monoSpeculation_ = false;
  mozilla::HashNumber monoICHash_ = 0;

 public:
  BaselineSnapshot(JSScript* script,
                   GlobalLexicalEnvironmentObject* globalLexical,
                   JSObject* globalThis, uint32_t baseWarmUpThreshold,
                   bool isIonCompileable, bool compileDebugInstrumentation)
      : script_(script),
        globalLexical_(globalLexical),
        globalThis_(globalThis),
        callObjectTemplate_(nullptr),
        namedLambdaTemplate_(nullptr),
        nargs_(script->function() ? script->function()->nargs() : 0),
        baseWarmUpThreshold_(baseWarmUpThreshold),
        isIonCompileable_(isIonCompileable),
        compileDebugInstrumentation_(compileDebugInstrumentation) {
    if (script->needsFunctionEnvironmentObjects()) {
      auto [callObjectTemplate, namedLambdaTemplate] =
          script->jitScript()->functionEnvironmentTemplates(script->function());
      callObjectTemplate_.init(callObjectTemplate);
      namedLambdaTemplate_.init(namedLambdaTemplate);
    }
  }

  JSScript* script() const { return script_; }
  GlobalLexicalEnvironmentObject* globalLexical() const {
    return globalLexical_;
  }
  JSObject* globalThis() const { return globalThis_; }
  CallObject* callObjectTemplate() const { return callObjectTemplate_; }
  NamedLambdaObject* namedLambdaTemplate() const {
    return namedLambdaTemplate_;
  }
  size_t nargs() const { return nargs_; }
  uint32_t baseWarmUpThreshold() const { return baseWarmUpThreshold_; }
  bool isIonCompileable() const { return isIonCompileable_; }
  bool compileDebugInstrumentation() const {
    return compileDebugInstrumentation_;
  }
  MonoScriptReason monoReason() const { return monoReason_; }
  MonoIC* monoICs() const { return monoICs_; }
  bool monoSpeculation() const { return monoSpeculation_; }
  mozilla::HashNumber monoICHash() const { return monoICHash_; }
  void setMonoSpeculation(mozilla::HashNumber monoICHash) {
    monoSpeculation_ = true;
    monoICHash_ = monoICHash;
  }

  // Decides whether the script is compiled monomorphic, and if so takes its
  // ICs into alloc. Returns false on OOM.
  [[nodiscard]] bool takeMonoICs(JSContext* cx, LifoAlloc& alloc);

  void trace(JSTracer* trc);
};

class OffThreadBaselineSnapshot
    : public BaselineSnapshot,
      public mozilla::LinkedListElement<OffThreadBaselineSnapshot> {
 public:
  OffThreadBaselineSnapshot(JSScript* script,
                            GlobalLexicalEnvironmentObject* globalLexical,
                            JSObject* globalThis, uint32_t baseWarmUpThreshold,
                            bool isIonCompileable,
                            bool compileDebugInstrumentation)
      : BaselineSnapshot(script, globalLexical, globalThis, baseWarmUpThreshold,
                         isIonCompileable, compileDebugInstrumentation) {}
  explicit OffThreadBaselineSnapshot(BaselineSnapshot other)
      : BaselineSnapshot(other) {}

  bool compileOffThread(TempAllocator& temp, CompileRealm* realm);

 private:
  mozilla::Maybe<OffThreadMacroAssembler> masm_;
  mozilla::Maybe<BaselineCompiler> compiler_;

  friend class BaselineCompileTask;
};

using BaselineSnapshotList = mozilla::LinkedList<OffThreadBaselineSnapshot>;

class BaselineCompileTask final : public HelperThreadTask {
 public:
  BaselineCompileTask(CompileRealm* realm, LifoAlloc* alloc,
                      BaselineSnapshotList&& snapshots)
      : realm_(realm), alloc_(alloc), snapshots_(std::move(snapshots)) {
#ifdef DEBUG
    for (auto* snapshot : snapshots_) {
      MOZ_ASSERT(CompileRealm::get(snapshot->script()->realm()) == realm);
    }
#endif
  }

  static bool OffThreadBaselineCompilationAvailable(JSContext* cx,
                                                    JSScript* script,
                                                    bool isEager = false);

  ThreadType threadType() override { return THREAD_TYPE_BASELINE; }
  void runTask();
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;

  void markScriptsAsCompiling();

  void finishOnMainThread(JSContext* cx);

  JSRuntime* runtimeFromAnyThread() const {
    return firstScript()->runtimeFromAnyThread();
  }
  Zone* zoneFromAnyThread() const { return firstScript()->zoneFromAnyThread(); }
  bool scriptMatches(JSScript* script) {
    for (auto* snapshot : snapshots_) {
      if (snapshot->script() == script) {
        return true;
      }
    }
    return false;
  }

  const char* getName() override { return "BaselineCompileTask"; }

  void trace(JSTracer* trc);

  static void FinishOffThreadTask(BaselineCompileTask* task);

  bool failed() const { return failed_; }

  // Whether the task compiles one script monomorphic before it is warm.
  bool isMonoSpeculation() const {
    return snapshots_.getFirst()->monoSpeculation();
  }
  JSScript* monoSpeculationScript() const { return firstScript(); }

  // Whether the task is parked, so its script is no longer marked as
  // compiling.
  bool parked() const { return parked_; }
  void setParked() { parked_ = true; }

 private:
  // All scripts are in the same realm, so we can use an arbitrary script
  // to access the realm/zone/runtime.
  JSScript* firstScript() const { return snapshots_.getFirst()->script(); }

  CompileRealm* realm_;
  LifoAlloc* alloc_;
  BaselineSnapshotList snapshots_;

  bool failed_ = false;
  bool parked_ = false;
};

// Uses the parked speculative monomorphic compile of the script, if any, unless
// the code must have debug instrumentation, in which case it is dropped.
// Returns whether the script has a BaselineScript after.
bool LinkParkedMonoBaseline(JSContext* cx, JSScript* script,
                            bool needsDebugInstrumentation);

}  // namespace js::jit

#endif /* jit_BaselineCompileTask_h */
