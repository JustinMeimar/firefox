/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MonoBaseline.h"

#include "gc/GC.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCloner.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRReader.h"
#include "jit/CacheIRWriter.h"
#include "jit/Invalidation.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/JSJitFrameIter.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICHelpers.h"
#include "util/Memory.h"
#include "vm/HelperThreads.h"

#include "jit/JitScript-inl.h"
#include "jit/JSJitFrameIter-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

mozilla::Maybe<MonoOperands> js::jit::MonoOperandsFor(CacheKind kind,
                                                      uint32_t popped) {
  switch (kind) {
    case CacheKind::GetProp:
    case CacheKind::TypeOf:
    case CacheKind::TypeOfEq:
    case CacheKind::ToPropertyKey:
    case CacheKind::GetIterator:
    case CacheKind::OptimizeSpreadCall:
    case CacheKind::OptimizeGetIterator:
    case CacheKind::ToBool:
    case CacheKind::UnaryArith:
      if (popped == 1) {
        return mozilla::Some(MonoOperands::R0);
      }
      break;
    case CacheKind::Compare:
    case CacheKind::GetElem:
    case CacheKind::In:
    case CacheKind::HasOwn:
    case CacheKind::InstanceOf:
    case CacheKind::BinaryArith:
      if (popped == 2) {
        return mozilla::Some(MonoOperands::R0R1);
      }
      break;
    case CacheKind::SetProp:
      if (popped == 1) {
        return mozilla::Some(MonoOperands::SetProp);
      }
      break;
    case CacheKind::SetElem:
      if (popped == 2) {
        return mozilla::Some(MonoOperands::SetElem);
      }
      break;
    case CacheKind::GetPropSuper:
    case CacheKind::GetElemSuper:
    case CacheKind::CheckPrivateField:
    case CacheKind::CloseIter:
      return mozilla::Nothing();
    default:
      break;
  }
  if (popped == 0) {
    return mozilla::Some(MonoOperands::InPlace);
  }
  return mozilla::Nothing();
}

// Whether the stub can be copied: it holds no nursery cell, which would need
// the store buffer to point into the copy, and no ICScript, which trial
// inlining may free.
static bool CanCopyStub(ICCacheIRStub* stub) {
  const CacheIRStubInfo* info = stub->stubInfo();
  const uint8_t* data = stub->stubDataStart();
  size_t offset = 0;
  for (uint32_t field = 0;; field++) {
    StubField::Type type = info->fieldType(field);
    switch (type) {
      case StubField::Type::Limit:
        return true;
      case StubField::Type::ICScript:
        return false;
      case StubField::Type::JSObject:
      case StubField::Type::WeakObject:
      case StubField::Type::String:
      case StubField::Type::Symbol: {
        auto* cell =
            reinterpret_cast<gc::Cell*>(info->getStubRawWord(data, offset));
        if (IsInsideNursery(cell)) {
          return false;
        }
        break;
      }
      case StubField::Type::Value:
      case StubField::Type::WeakValue: {
        Value v = Value::fromRawBits(info->getStubRawInt64(data, offset));
        if (v.isGCThing() && IsInsideNursery(v.toGCThing())) {
          return false;
        }
        break;
      }
      default:
        break;
    }
    offset += StubField::sizeInBytes(type);
  }
}

// Ops whose Baseline code reads stub data without a constant form.
static bool CanInlineOp(CacheOp op) {
  switch (op) {
    case CacheOp::AddAndStoreFixedSlot:
    case CacheOp::AddAndStoreDynamicSlot:
    case CacheOp::AllocateAndStoreDynamicSlot:
    case CacheOp::GuardDOMExpandoMissingOrGuardShape:
    case CacheOp::LoadDOMExpandoValueGuardGeneration:
    case CacheOp::HasClassResult:
    case CacheOp::HasShapeResult:
    case CacheOp::NewArrayObjectResult:
    case CacheOp::NewPlainObjectResult:
    case CacheOp::NewFunctionCloneResult:
    case CacheOp::MetaCreateThis:
    case CacheOp::ProxyGetResult:
    case CacheOp::ProxySet:
    case CacheOp::CallNativeGetterResult:
    case CacheOp::CallDOMGetterResult:
    case CacheOp::CallNativeSetter:
    case CacheOp::CallDOMSetter:
      return false;
    default:
      return true;
  }
}

MonoNoInline js::jit::WhyNotInlineMonoStub(ICCacheIRStub* stub, CacheOp* op) {
  const CacheIRStubInfo* info = stub->stubInfo();
  if (info->makesGCCalls()) {
    return MonoNoInline::GCCalls;
  }
  if (!CanCopyStub(stub)) {
    return MonoNoInline::NoCopy;
  }
  CacheIRReader reader(info);
  while (reader.more()) {
    *op = reader.readOp();
    if (!CanInlineOp(*op)) {
      return MonoNoInline::Op;
    }
    reader.skip(CacheIROpInfos[size_t(*op)].argLength);
  }
  return MonoNoInline::None;
}

bool js::jit::CanInlineMonoStub(ICCacheIRStub* stub) {
  CacheOp op;
  return WhyNotInlineMonoStub(stub, &op) == MonoNoInline::None;
}

bool js::jit::EmitInlinedMonoStub(JSContext* cx, MacroAssembler& masm,
                                  TempAllocator& alloc, ICCacheIRStub* stub,
                                  mozilla::Span<const JSValueType> inputTypes,
                                  InlinedInputVector& origInputs,
                                  InlinedFailureVector& failures,
                                  bool* inlined) {
  *inlined = false;
  MOZ_ASSERT(CanInlineMonoStub(stub));
  const CacheIRStubInfo* info = stub->stubInfo();
  CacheKind kind = info->kind();

  CacheIRWriter writer(cx);
  for (size_t i = 0; i < NumInputsForCacheKind(kind); i++) {
    writer.setInputOperandId(i);
  }
  CacheIRReader reader(info);
  CacheIRCloner cloner(stub);
  while (reader.more()) {
    cloner.cloneOp(reader.readOp(), reader, writer);
  }
  if (writer.failed()) {
    return true;
  }

  // The code holds weak edges strongly, so mark them if we are marking.
  Zone* zone = cx->zone();
  if (zone->needsMarkingBarrier()) {
    TraceCacheIRStub(zone->barrierTracer(),
                     reinterpret_cast<MonoStubCopy*>(stub), info);
  }

  BaselineCacheIRCompiler compiler(cx, alloc, writer, 0, &masm);
  if (!compiler.init(kind) ||
      !compiler.compileInline(inputTypes, origInputs, failures)) {
    return false;
  }
  *inlined = true;
  return true;
}

void js::jit::EmitMonoExit(MacroAssembler& masm, MonoOperands operands,
                           uint32_t pcOffset, TrampolinePtr exitTail) {
  Address top(masm.getStackPointer(), 0);
  switch (operands) {
    case MonoOperands::InPlace:
      break;
    case MonoOperands::R0:
      masm.pushValue(R0);
      break;
    case MonoOperands::R0R1:
      masm.pushValue(R0);
      masm.pushValue(R1);
      break;
    case MonoOperands::SetProp:
      masm.storeValue(R0, top);
      masm.pushValue(R1);
      break;
    case MonoOperands::SetElem:
      masm.loadValue(top, R2);
      masm.storeValue(R0, top);
      masm.pushValue(R1);
      masm.pushValue(R2);
      break;
    case MonoOperands::Count:
      MOZ_CRASH();
  }
  masm.move32(Imm32(pcOffset), ICStubReg);
  masm.jump(exitTail);
}

MonoStub* MonoStub::New(JSContext* cx, ICCacheIRStub* stub, uint32_t pcOffset,
                        MonoOperands operands, bool* oom) {
  *oom = false;
  if (!CanCopyStub(stub)) {
    return nullptr;
  }

  const CacheIRStubInfo* info = stub->stubInfo();

  // The copy holds weak edges strongly, so mark them if we are marking.
  Zone* zone = cx->zone();
  if (zone->needsMarkingBarrier()) {
    TraceWeakCacheIRStub(zone->barrierTracer(), stub, info);
  }

  size_t bytes = offsetOfStub() + info->stubDataOffset() + info->stubDataSize();
  uint8_t* mem = js_pod_malloc<uint8_t>(bytes);
  if (!mem) {
    *oom = true;
    return nullptr;
  }
  auto* monoStub = new (mem)
      MonoStub(Exit{cx->runtime()->jitRuntime()->monoStubExit(operands).value,
                    pcOffset, false});
  auto* copy = new (monoStub->stub()) ICCacheIRStub(stub->jitCode(), info);
  CopyCacheIRStubData(info, stub->stubDataStart(), copy->stubDataStart());
  copy->setNext(reinterpret_cast<ICStub*>(&monoStub->exit_));
  return monoStub;
}

void MonoStub::DestroyList(MonoStub* list) {
  while (list) {
    MonoStub* next = list->next_;
    js_free(list);
    list = next;
  }
}

void MonoStub::TraceList(JSTracer* trc, MonoStub* list) {
  for (MonoStub* monoStub = list; monoStub; monoStub = monoStub->next_) {
    if (monoStub->exit_.discarded) {
      continue;
    }
    ICCacheIRStub* stub = monoStub->stub();
    JitCode* code = stub->jitCode();
    TraceManuallyBarrieredEdge(trc, &code, "mono-stub-code");
    TraceCacheIRStub(trc, reinterpret_cast<MonoStubCopy*>(stub),
                     stub->stubInfo());
  }
}

void MonoStub::DiscardList(JSRuntime* rt, MonoStub* list) {
  uint8_t* fail = rt->jitRuntime()->monoStubFail().value;
  for (MonoStub* monoStub = list; monoStub; monoStub = monoStub->next_) {
    monoStub->stub()->setStubCode(fail);
    monoStub->exit_.discarded = true;
  }
}

void js::jit::DiscardMonoStubs(JSRuntime* rt, JSScript* script) {
  BaselineScript* baselineScript = script->baselineScript();
  MOZ_ASSERT(baselineScript->isMono());
  if (baselineScript->monoStatsId()) {
    LogMonoScriptEvent("invalidate", script, baselineScript);
  }
  baselineScript->discardMonoStubs(rt);
  if (!baselineScript->monoFailed()) {
    baselineScript->setMonoFailed();
    script->updateJitCodeRaw(rt);
  }
}

bool js::jit::IsMonomorphicScript(JSScript* script) {
  ICScript* icScript = script->jitScript()->icScript();
  bool hasStub = false;
  for (size_t i = 0; i < icScript->numICEntries(); i++) {
    if (icScript->icEntry(i).firstStub()->isFallback()) {
      continue;
    }
    const ICState& state = icScript->fallbackStub(i)->state();
    if (state.mode() != ICState::Mode::Specialized ||
        state.numOptimizedStubs() != 1 || state.hasFailures()) {
      return false;
    }
    hasStub = true;
  }
  return hasStub;
}

uint8_t* js::jit::MonoBaselineExit(BaselineFrame* frame, uint32_t pcOffset) {
  AutoUnsafeCallWithABI unsafe;
  JSContext* cx = TlsContext.get();
  JSScript* script = frame->script();
  JitScript* jitScript = script->jitScript();
  BaselineScript* baselineScript = script->baselineScript();
  MOZ_ASSERT(baselineScript->isMono());

  if (baselineScript->monoStatsId()) {
    LogMonoExit(script, baselineScript, pcOffset);
  }

  if (!baselineScript->monoFailed()) {
    baselineScript->setMonoFailed();
    jitScript->setMonoFailed();
    script->updateJitCodeRaw(cx->runtime());
    jitScript->resetWarmUpCount(0);
  }

  AutoSuppressProfilerSampling suppressProfilerSampling(cx);
  frame->switchFromJitToInterpreter(cx, script->offsetToPC(pcOffset));
  return cx->runtime()
      ->jitRuntime()
      ->baselineInterpreter()
      .interpretOpAddr()
      .value;
}

static bool RunsBaselineCode(JSContext* cx, JSScript* script) {
  for (JitActivationIterator iter(cx); !iter.done(); ++iter) {
    for (OnlyJSJitFrameIter frames(iter); !frames.done(); ++frames) {
      const JSJitFrameIter& frame = frames.frame();
      if (frame.isBaselineJS() && frame.script() == script &&
          !frame.baselineFrame()->runningInInterpreter()) {
        return true;
      }
    }
  }
  return false;
}

bool js::jit::MaybeDiscardFailedMonoBaseline(JSContext* cx, JSScript* script) {
  MOZ_ASSERT(script->baselineScript()->monoFailed());
  if (RunsBaselineCode(cx, script)) {
    return false;
  }
  if (script->hasIonScript()) {
    Invalidate(cx, script, /* resetUses = */ false);
  }
  CancelOffThreadIonCompile(script);
  if (script->baselineScript()->monoStatsId()) {
    LogMonoScriptEvent("discard", script, script->baselineScript());
  }
  BaselineScript* baselineScript =
      script->jitScript()->clearBaselineScript(cx->gcContext(), script);
  BaselineScript::Destroy(cx->gcContext(), baselineScript);
  return true;
}

void JitRuntime::generateMonoStubExits(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateMonoStubExits");

  // The code of discarded stubs, which fails right away.
  monoStubFailOffset_ = startTrampolineCode(masm);
  EmitStubGuardFailure(masm);

  // Stubs jump to these with the return address of their call on the stack,
  // their inputs restored and the stub register pointing to the exit. Inline
  // stubs jump to the tail with their operands put back and the stub register
  // holding the pc offset.
  Label exit;
  for (size_t i = 0; i < size_t(MonoOperands::Count); i++) {
    monoStubExitOffsets_[MonoOperands(i)] = startTrampolineCode(masm);
#ifndef JS_USE_LINK_REGISTER
    masm.addToStackPtr(Imm32(sizeof(void*)));
#endif
    Address top(masm.getStackPointer(), 0);
    switch (MonoOperands(i)) {
      case MonoOperands::InPlace:
        break;
      case MonoOperands::R0:
        masm.pushValue(R0);
        break;
      case MonoOperands::R0R1:
        masm.pushValue(R0);
        masm.pushValue(R1);
        break;
      case MonoOperands::SetProp:
        masm.storeValue(R0, top);
        masm.pushValue(R1);
        break;
      case MonoOperands::SetElem:
        masm.loadValue(top, R2);
        masm.storeValue(R0, top);
        masm.pushValue(R1);
        masm.pushValue(R2);
        break;
      case MonoOperands::Count:
        MOZ_CRASH();
    }
    masm.load32(Address(ICStubReg, MonoStub::offsetOfExitPcOffset()),
                ICStubReg);
    masm.jump(&exit);
  }

  masm.bind(&exit);
  monoStubExitTailOffset_ = uint32_t(exit.offset());
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  regs.takeUnchecked(ICStubReg);
  Register frame = regs.takeAny();
  Register temp = regs.takeAny();

  masm.loadBaselineFramePtr(FramePointer, frame);
  using Fn = uint8_t* (*)(BaselineFrame*, uint32_t);
  masm.setupUnalignedABICall(temp);
  masm.passABIArg(frame);
  masm.passABIArg(ICStubReg);
  masm.callWithABI<Fn, MonoBaselineExit>();
  masm.jump(ReturnReg);
}
