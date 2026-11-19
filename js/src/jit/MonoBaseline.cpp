/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MonoBaseline.h"

#include "gc/GC.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRCloner.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRWriter.h"
#include "jit/Invalidation.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/JSJitFrameIter.h"
#include "jit/Linker.h"
#include "jit/MacroAssembler.h"
#include "js/UniquePtr.h"
#include "vm/HelperThreads.h"

#include "jit/JitScript-inl.h"
#include "jit/JSJitFrameIter-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

InlinedStub* InlinedStub::New(const CacheIRWriter& writer, CacheKind kind) {
  UniquePtr<CacheIRStubInfo> stubInfo(CacheIRStubInfo::New(
      kind, ICStubEngine::Baseline,
      /* makesGCCalls = */ false, stubDataOffset(), writer));
  if (!stubInfo) {
    return nullptr;
  }
  uint8_t* mem =
      js_pod_malloc<uint8_t>(stubDataOffset() + writer.stubDataSize());
  if (!mem) {
    return nullptr;
  }
  auto* stub = new (mem) InlinedStub(stubInfo.release());
  writer.copyStubData(mem + stubDataOffset());
  return stub;
}

void InlinedStub::Destroy(InlinedStub* stub) {
  js_free(stub->stubInfo_);
  stub->~InlinedStub();
  js_free(stub);
}

void InlinedStub::DestroyList(InlinedStub* list) {
  while (list) {
    InlinedStub* next = list->next();
    Destroy(list);
    list = next;
  }
}

void InlinedStub::trace(JSTracer* trc) {
  TraceCacheIRStub(trc, this, stubInfo_);
}

void js::jit::TraceInlinedStubs(JSTracer* trc, InlinedStub* list) {
  for (InlinedStub* stub = list; stub; stub = stub->next()) {
    stub->trace(trc);
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
    case CacheKind::Call:
      return mozilla::Nothing();
    default:
      break;
  }
  if (popped == 0) {
    return mozilla::Some(MonoOperands::InPlace);
  }
  return mozilla::Nothing();
}

// Whether a stub field holds a nursery cell, which would need the store buffer
// to point into the stub data.
static bool HasNurseryStubField(const CacheIRWriter& writer) {
  for (size_t i = 0; i < writer.numStubFields(); i++) {
    const StubField& field = writer.stubField(i);
    switch (field.type()) {
      case StubField::Type::JSObject:
      case StubField::Type::WeakObject:
      case StubField::Type::String:
      case StubField::Type::Symbol:
        if (IsInsideNursery(reinterpret_cast<gc::Cell*>(field.asWord()))) {
          return true;
        }
        break;
      case StubField::Type::Value:
      case StubField::Type::WeakValue: {
        Value v = Value::fromRawBits(field.asInt64());
        if (v.isGCThing() && IsInsideNursery(v.toGCThing())) {
          return true;
        }
        break;
      }
      default:
        break;
    }
  }
  return false;
}

bool js::jit::EmitInlinedStub(JSContext* cx, MacroAssembler& masm,
                              TempAllocator& alloc, ICCacheIRStub* stub,
                              CacheKind kind, Label* failure,
                              InlinedStub** inlined) {
  *inlined = nullptr;
  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  if (stubInfo->makesGCCalls()) {
    return true;
  }

  CacheIRWriter writer(cx);
  for (size_t i = 0; i < NumInputsForCacheKind(kind); i++) {
    writer.setInputOperandId(i);
  }
  CacheIRReader reader(stubInfo);
  CacheIRCloner cloner(stub);
  while (reader.more()) {
    cloner.cloneOp(reader.readOp(), reader, writer);
  }
  if (writer.failed() || HasNurseryStubField(writer)) {
    return true;
  }

  // The copy holds weak edges strongly, so mark them if we are marking.
  Zone* zone = cx->zone();
  if (zone->needsMarkingBarrier()) {
    TraceWeakCacheIRStub(zone->barrierTracer(), stub, stubInfo);
  }

  InlinedStub* newStub = InlinedStub::New(writer, kind);
  if (!newStub) {
    return false;
  }
  masm.movePtr(ImmPtr(newStub), ICStubReg);
  BaselineCacheIRCompiler compiler(cx, alloc, writer,
                                   InlinedStub::stubDataOffset(), &masm);
  if (!compiler.init(kind) || !compiler.compileInline(failure)) {
    InlinedStub::Destroy(newStub);
    return false;
  }
  *inlined = newStub;
  return true;
}

void js::jit::EmitMonoExit(MacroAssembler& masm, MonoOperands operands,
                           uint32_t pcOffset, TrampolinePtr exitStub) {
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
  }
  masm.move32(Imm32(pcOffset), ICStubReg);
  masm.jump(exitStub);
}

uint8_t* js::jit::MonoBaselineExit(BaselineFrame* frame, uint32_t pcOffset) {
  AutoUnsafeCallWithABI unsafe;
  JSContext* cx = TlsContext.get();
  JSScript* script = frame->script();
  JitScript* jitScript = script->jitScript();
  BaselineScript* baselineScript = script->baselineScript();
  MOZ_ASSERT(baselineScript->isMono());

  if (!baselineScript->monoFailed()) {
    baselineScript->setMonoFailed();
    jitScript->setMonoFailed();
    script->updateJitCodeRaw(cx->runtime());
    jitScript->resetWarmUpCount(0);
  }

  frame->switchFromJitToInterpreter(cx, script->offsetToPC(pcOffset));
  return cx->runtime()
      ->jitRuntime()
      ->baselineInterpreter()
      .interpretOpAddr()
      .value;
}

bool js::jit::HasFailedMonoBaseline(JSScript* script) {
  return script->hasBaselineScript() && script->baselineScript()->monoFailed();
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
  MOZ_ASSERT(HasFailedMonoBaseline(script));
  if (RunsBaselineCode(cx, script)) {
    return false;
  }
  if (script->hasIonScript()) {
    Invalidate(cx, script, /* resetUses = */ false);
  }
  CancelOffThreadIonCompile(script);
  BaselineScript* baselineScript =
      script->jitScript()->clearBaselineScript(cx->gcContext(), script);
  BaselineScript::Destroy(cx->gcContext(), baselineScript);
  return true;
}

void JitRuntime::generateMonoBaselineExitStub(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateMonoBaselineExitStub");
  monoBaselineExitStubOffset_ = startTrampolineCode(masm);

  // ICStubReg holds the pc offset of the op.
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
