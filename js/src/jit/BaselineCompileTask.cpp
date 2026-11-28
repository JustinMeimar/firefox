/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCompileTask.h"

#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/MonoBaseline.h"
#include "jit/MonoStats.h"
#include "vm/HelperThreadState.h"

#include "jit/JitHints-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

void BaselineCompileTask::runHelperThreadTask(
    AutoLockHelperThreadState& locked) {
  {
    AutoUnlockHelperThreadState unlock(locked);
    runTask();
  }

  FinishOffThreadBaselineCompile(this, locked);

  // Ping the main thread so that the compiled code can be incorporated at
  // the next interrupt callback.
  runtimeFromAnyThread()->mainContextFromAnyThread()->requestInterrupt(
      InterruptReason::AttachOffThreadCompilations);
}

// Debugging RAII class which marks the current thread as performing
// an offthread baseline compilation.
class MOZ_RAII AutoEnterBaselineBackend {
 public:
  AutoEnterBaselineBackend() {
#ifdef DEBUG
    JitContext* jcx = GetJitContext();
    jcx->enterBaselineBackend();
#endif
  }

#ifdef DEBUG
  ~AutoEnterBaselineBackend() {
    JitContext* jcx = GetJitContext();
    jcx->leaveBaselineBackend();
  }
#endif
};

void BaselineCompileTask::markScriptsAsCompiling() {
  for (auto* snapshot : snapshots_) {
    JSScript* script = snapshot->script();
    script->jitScript()->setIsBaselineCompiling(script);
  }
}

bool OffThreadBaselineSnapshot::compileOffThread(TempAllocator& temp,
                                                 CompileRealm* realm) {
  masm_.emplace(temp, realm);
  compiler_.emplace(temp, realm->runtime(), masm_.ref(), this);

  if (!compiler_->init()) {
    return false;
  }
  MethodStatus status = compiler_->compileOffThread();
  return status != Method_Error;
}

void BaselineCompileTask::runTask() {
  jit::JitContext jctx(realm_->runtime());
  AutoEnterBaselineBackend enter;

  TempAllocator* temp = alloc_->new_<TempAllocator>(alloc_);
  if (!temp) {
    failed_ = true;
    return;
  }

  for (auto* snapshot : snapshots_) {
    if (!snapshot->compileOffThread(*temp, realm_)) {
      failed_ = true;
      return;
    }
  }
}

/* static */
void BaselineCompileTask::FinishOffThreadTask(BaselineCompileTask* task) {
  for (auto* snapshot : task->snapshots_) {
    JSScript* script = snapshot->script();
    if (task->parked_) {
      if (script->hasJitScript()) {
        script->jitScript()->setMonoSpeculationParked(false);
      }
    } else if (script->isBaselineCompilingOffThread()) {
      script->jitScript()->clearIsBaselineCompiling(script);
    }
    snapshot->compiler_.reset();
    snapshot->masm_.reset();
  }

  // The task is allocated into its LifoAlloc, so destroying that will
  // destroy the task and all other data accumulated during compilation.
  js_delete(task->alloc_);
}

void BaselineCompileTask::finishOnMainThread(JSContext* cx) {
  AutoRealm ar(cx, firstScript());
  for (auto* snapshot : snapshots_) {
    if (snapshot->monoSpeculation() &&
        (snapshot->script()->isDebuggee() ||
         HashMonoICs(snapshot->script()) != snapshot->monoICHash())) {
      LogMonoSpeculation("drop", snapshot->script());
      continue;
    }
    if (snapshot->monoSpeculation()) {
      LogMonoSpeculation("use", snapshot->script());
      if (!JitOptions.disableJitHints &&
          cx->runtime()->jitRuntime()->hasJitHintsMap()) {
        cx->runtime()->jitRuntime()->getJitHintsMap()->setEagerBaselineHint(
            snapshot->script());
      }
    }
    if (!snapshot->compiler_->finishCompile(cx)) {
      cx->recoverFromOutOfMemory();
    }
  }
}

void js::AttachFinishedBaselineCompilations(JSContext* cx,
                                            AutoLockHelperThreadState& lock) {
  JSRuntime* rt = cx->runtime();

  while (true) {
    GlobalHelperThreadState::BaselineCompileTaskVector& finished =
        HelperThreadState().baselineFinishedList(lock);

    // Find a finished task for this runtime.
    bool found = false;
    for (size_t i = 0; i < finished.length(); i++) {
      BaselineCompileTask* task = finished[i];
      if (task->runtimeFromAnyThread() != rt) {
        continue;
      }
      found = true;

      HelperThreadState().remove(finished, &i);
      rt->jitRuntime()->numFinishedOffThreadTasksRef(lock)--;

      // Speculative compiles that finish before their script is warm wait for
      // it to be.
      if (!task->failed() && task->isMonoSpeculation()) {
        JSScript* script = task->monoSpeculationScript();
        if (script->getWarmUpCount() <= JitOptions.baselineJitWarmUpThreshold &&
            HelperThreadState().baselineParkedList(lock).append(task)) {
          script->jitScript()->clearIsBaselineCompiling(script);
          script->jitScript()->setMonoSpeculationParked(true);
          task->setParked();
          LogMonoSpeculation("park", script);
          continue;
        }
      }

      {
        if (!task->failed()) {
          AutoUnlockHelperThreadState unlock(lock);
          task->finishOnMainThread(cx);
        }
        BaselineCompileTask::FinishOffThreadTask(task);
      }
    }
    if (!found) {
      break;
    }
  }
}

bool jit::LinkParkedMonoBaseline(JSContext* cx, JSScript* script,
                                 bool needsDebugInstrumentation) {
  MOZ_ASSERT(script->jitScript()->monoSpeculationParked());
  BaselineCompileTask* task = nullptr;
  {
    AutoLockHelperThreadState lock;
    GlobalHelperThreadState::BaselineCompileTaskVector& parked =
        HelperThreadState().baselineParkedList(lock);
    for (size_t i = 0; i < parked.length(); i++) {
      if (parked[i]->scriptMatches(script)) {
        task = parked[i];
        HelperThreadState().remove(parked, &i);
        break;
      }
    }
  }
  script->jitScript()->setMonoSpeculationParked(false);
  if (!task) {
    return false;
  }
  if (needsDebugInstrumentation) {
    LogMonoSpeculation("drop", script);
  } else {
    task->finishOnMainThread(cx);
  }
  BaselineCompileTask::FinishOffThreadTask(task);
  return script->hasBaselineScript();
}

bool BaselineSnapshot::takeMonoICs(JSContext* cx, LifoAlloc& alloc) {
  monoReason_ = MonoReasonFor(cx, script(), compileDebugInstrumentation_);
  if (monoReason_ != MonoScriptReason::Mono) {
    return true;
  }
  monoICs_ = TakeMonoICs(cx, script(), alloc);
  if (!monoICs_) {
    return false;
  }
  numMonoICs_ = script()->jitScript()->numICEntries();
  return true;
}

void BaselineSnapshot::trace(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, script_, "baseline-snapshot-script");
  TraceOffthreadGCPtr(trc, globalLexical_, "baseline-snapshot-lexical");
  TraceOffthreadGCPtr(trc, globalThis_, "baseline-snapshot-this");
  if (callObjectTemplate_) {
    TraceOffthreadGCPtr(trc, callObjectTemplate_,
                        "baseline-snapshot-call-object-template");
  }
  if (namedLambdaTemplate_) {
    TraceOffthreadGCPtr(trc, namedLambdaTemplate_,
                        "baseline-snapshot-named-lambda-template");
  }
  if (monoICs_) {
    TraceMonoICs(trc, monoICs_, numMonoICs_);
  }
}

void BaselineCompileTask::trace(JSTracer* trc) {
  if (!realm_->runtime()->runtimeMatches(trc->runtime())) {
    return;
  }
  for (auto* snapshot : snapshots_) {
    snapshot->trace(trc);
  }
}
