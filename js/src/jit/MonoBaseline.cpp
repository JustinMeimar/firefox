/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MonoBaseline.h"

#include <algorithm>
#include <bit>
#include <iterator>

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
#include "jit/MonoTemplates.h"
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

// Scans the CacheIR and fields of the info once for what monomorphic compiles
// need to know about its stubs.
static const CacheIRStubInfo::MonoBaselineCache& ScanStubInfo(
    const CacheIRStubInfo* info) {
  CacheIRStubInfo::MonoBaselineCache& cache = info->monoBaselineCache();
  if (cache.cacheIRScanned) {
    return cache;
  }

  cache.allOpsInlinable = true;
  std::fill(std::begin(cache.guardedInputTypes),
            std::end(cache.guardedInputTypes), JSVAL_TYPE_UNKNOWN);
  CacheIRReader reader(info);
  while (reader.more()) {
    CacheOp op = reader.readOp();
    if (cache.allOpsInlinable && !CanInlineOp(op)) {
      cache.allOpsInlinable = false;
      cache.noInlineOp = op;
    }
    ValOperandId input;
    JSValueType type;
    switch (op) {
      case CacheOp::GuardToObject:
        input = reader.argsForGuardToObject().inputId;
        type = JSVAL_TYPE_OBJECT;
        break;
      case CacheOp::GuardToInt32:
        input = reader.argsForGuardToInt32().inputId;
        type = JSVAL_TYPE_INT32;
        break;
      case CacheOp::GuardToString:
        input = reader.argsForGuardToString().inputId;
        type = JSVAL_TYPE_STRING;
        break;
      case CacheOp::GuardToSymbol:
        input = reader.argsForGuardToSymbol().inputId;
        type = JSVAL_TYPE_SYMBOL;
        break;
      case CacheOp::GuardToBigInt:
        input = reader.argsForGuardToBigInt().inputId;
        type = JSVAL_TYPE_BIGINT;
        break;
      case CacheOp::GuardToBoolean:
        input = reader.argsForGuardToBoolean().inputId;
        type = JSVAL_TYPE_BOOLEAN;
        break;
      case CacheOp::GuardIsNull:
        input = reader.argsForGuardIsNull().inputId;
        type = JSVAL_TYPE_NULL;
        break;
      case CacheOp::GuardIsUndefined:
        input = reader.argsForGuardIsUndefined().inputId;
        type = JSVAL_TYPE_UNDEFINED;
        break;
      case CacheOp::GuardNonDoubleType: {
        auto args = reader.argsForGuardNonDoubleType();
        input = args.inputId;
        type = JSValueType(args.type);
        break;
      }
      case CacheOp::CallScriptedFunction:
      case CacheOp::CallBoundScriptedFunction:
      case CacheOp::CallScriptedGetterResult:
      case CacheOp::CallScriptedSetter:
        cache.callsScriptedFunction = true;
        reader.skip(CacheIROpInfos[size_t(op)].argLength);
        continue;
      default:
        reader.skip(CacheIROpInfos[size_t(op)].argLength);
        continue;
    }
    if (input.id() < CacheIRStubInfo::MonoBaselineCache::NumGuardedInputs) {
      cache.guardedInputTypes[input.id()] = type;
    }
  }

  for (uint32_t field = 0;; field++) {
    StubField::Type type = info->fieldType(field);
    if (type == StubField::Type::Limit) {
      break;
    }
    switch (type) {
      case StubField::Type::ICScript:
        cache.hasICScriptStubField = true;
        break;
      case StubField::Type::JSObject:
      case StubField::Type::WeakObject:
      case StubField::Type::String:
      case StubField::Type::Symbol:
      case StubField::Type::Value:
      case StubField::Type::WeakValue:
        cache.hasStubFieldThatCanBeInNursery = true;
        break;
      default:
        break;
    }
  }

  cache.cacheIRScanned = true;
  return cache;
}

// Whether the stub can be copied: it holds no nursery cell, which would need
// the store buffer to point into the copy, and no ICScript, which trial
// inlining may free. A stub trial inlining may replace is called through the
// IC chain, so the callee gets the ICScript of the replacement and the stub
// counts the calls trial inlining reads.
static bool CanCopyStub(ICCacheIRStub* stub) {
  const CacheIRStubInfo* info = stub->stubInfo();
  const CacheIRStubInfo::MonoBaselineCache& cache = ScanStubInfo(info);
  if (cache.hasICScriptStubField || cache.callsScriptedFunction) {
    return false;
  }
  if (!cache.hasStubFieldThatCanBeInNursery) {
    return true;
  }
  const uint8_t* data = stub->stubDataStart();
  size_t offset = 0;
  for (uint32_t field = 0;; field++) {
    StubField::Type type = info->fieldType(field);
    switch (type) {
      case StubField::Type::Limit:
        return true;
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

MonoNoInline js::jit::WhyNotInlineMonoStub(ICCacheIRStub* stub, CacheOp* op) {
  const CacheIRStubInfo* info = stub->stubInfo();
  if (info->makesGCCalls()) {
    return MonoNoInline::GCCalls;
  }
  if (!CanCopyStub(stub)) {
    return MonoNoInline::NoCopy;
  }
  const CacheIRStubInfo::MonoBaselineCache& cache = ScanStubInfo(info);
  *op = cache.noInlineOp;
  return cache.allOpsInlinable ? MonoNoInline::None : MonoNoInline::Op;
}

bool js::jit::CanInlineMonoStub(ICCacheIRStub* stub) {
  CacheOp op;
  return WhyNotInlineMonoStub(stub, &op) == MonoNoInline::None;
}

// The code holds weak edges strongly, so mark them if we are marking.
static void BarrierInlinedMonoStub(JSContext* cx, ICCacheIRStub* stub) {
  Zone* zone = cx->zone();
  if (zone->needsMarkingBarrier()) {
    TraceCacheIRStub(zone->barrierTracer(),
                     reinterpret_cast<MonoStubCopy*>(stub), stub->stubInfo());
  }
}

static bool CompileInlinedMonoStub(JSContext* cx, MacroAssembler& masm,
                                   TempAllocator& alloc, ICCacheIRStub* stub,
                                   mozilla::Span<const JSValueType> inputTypes,
                                   InlinedInputVector& origInputs,
                                   InlinedFailureVector& failures) {
  const CacheIRStubInfo* info = stub->stubInfo();
  BaselineCacheIRCompiler compiler(
      cx, alloc, CacheIRSource(info, stub->stubDataStart()), 0, &masm);
  return compiler.init(info->kind()) &&
         compiler.compileInline(inputTypes, origInputs, failures);
}

bool js::jit::EmitInlinedMonoStub(JSContext* cx, MacroAssembler& masm,
                                  TempAllocator& alloc, ICCacheIRStub* stub,
                                  mozilla::Span<const JSValueType> inputTypes,
                                  InlinedInputVector& origInputs,
                                  InlinedFailureVector& failures) {
  MOZ_ASSERT(CanInlineMonoStub(stub));
  BarrierInlinedMonoStub(cx, stub);
  return CompileInlinedMonoStub(cx, masm, alloc, stub, inputTypes, origInputs,
                                failures);
}

bool js::jit::EmitInlinedMonoStubWithExitTable(
    JSContext* cx, MacroAssembler& masm, TempAllocator& alloc,
    ICCacheIRStub* stub, mozilla::Span<const JSValueType> inputTypes,
    CompactBufferWriter& states, MonoExitJumpVector& jumps,
    MonoTemplateUse* templateUse) {
  MOZ_ASSERT(CanInlineMonoStub(stub));
  BarrierInlinedMonoStub(cx, stub);

  *templateUse = MonoTemplateUse::None;
  if (JitOptions.baselineMonoTemplates) {
    MonoStubTemplates& templates =
        cx->runtime()->jitRuntime()->monoStubTemplates();
    if (!templates.emitStub(cx, masm, stub, inputTypes, states, jumps,
                            templateUse)) {
      return false;
    }
    if (*templateUse == MonoTemplateUse::Hit ||
        *templateUse == MonoTemplateUse::Built) {
      return true;
    }
  }

  InlinedInputVector origInputs;
  InlinedFailureVector failures;
  return CompileInlinedMonoStub(cx, masm, alloc, stub, inputTypes, origInputs,
                                failures) &&
         MonoExitTableWriter::encodeStates(masm, origInputs, failures, states,
                                           jumps);
}

void js::jit::NarrowMonoStubInputTypes(const CacheIRStubInfo* info,
                                       mozilla::Span<JSValueType> types) {
  static_assert(MonoMaxStackInputs <=
                CacheIRStubInfo::MonoBaselineCache::NumGuardedInputs);
  MOZ_ASSERT(types.size() <= MonoMaxStackInputs);
  const CacheIRStubInfo::MonoBaselineCache& cache = ScanStubInfo(info);
  for (size_t i = 0; i < types.size(); i++) {
    if (cache.guardedInputTypes[i] != JSVAL_TYPE_UNKNOWN) {
      types[i] = cache.guardedInputTypes[i];
    }
  }
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

bool js::jit::IsMonomorphicIC(ICScript* icScript, size_t entryIndex) {
  if (icScript->icEntry(entryIndex).firstStub()->isFallback()) {
    return false;
  }
  const ICState& state = icScript->fallbackStub(entryIndex)->state();
  return state.mode() == ICState::Mode::Specialized &&
         state.numOptimizedStubs() == 1 && !state.hasFailures();
}

bool js::jit::IsMonomorphicScript(JSScript* script) {
  ICScript* icScript = script->jitScript()->icScript();
  bool hasStub = false;
  for (size_t i = 0; i < icScript->numICEntries(); i++) {
    if (icScript->icEntry(i).firstStub()->isFallback()) {
      continue;
    }
    if (!IsMonomorphicIC(icScript, i)) {
      return false;
    }
    hasStub = true;
  }
  return hasStub;
}

bool js::jit::HasMonomorphicIC(JSScript* script) {
  ICScript* icScript = script->jitScript()->icScript();
  for (size_t i = 0; i < icScript->numICEntries(); i++) {
    if (IsMonomorphicIC(icScript, i)) {
      return true;
    }
  }
  return false;
}

static uint8_t* ContinueInInterpreter(BaselineFrame* frame,
                                      uint32_t pcOffset) {
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
  baselineScript->leaveMonoFrame();

  AutoSuppressProfilerSampling suppressProfilerSampling(cx);
  frame->switchFromJitToInterpreter(cx, script->offsetToPC(pcOffset));
  return cx->runtime()
      ->jitRuntime()
      ->baselineInterpreter()
      .interpretOpAddr()
      .value;
}

uint8_t* js::jit::MonoBaselineExit(BaselineFrame* frame, uint32_t pcOffset) {
  AutoUnsafeCallWithABI unsafe;
  return ContinueInInterpreter(frame, pcOffset);
}

MonoExitTable* MonoExitTable::New(mozilla::Span<const uint64_t> constants,
                                  const CompactBufferWriter& sites) {
  size_t bytes = sizeof(MonoExitTable) + constants.size() * sizeof(Value) +
                 sites.length();
  uint8_t* mem = js_pod_malloc<uint8_t>(bytes);
  if (!mem) {
    return nullptr;
  }
  auto* table = new (mem) MonoExitTable(constants.size(), sites.length());
  for (size_t i = 0; i < constants.size(); i++) {
    table->constants()[i] = Value::fromRawBits(constants[i]);
  }
  if (sites.length() != 0) {
    memcpy(const_cast<uint8_t*>(table->data()), sites.buffer(),
           sites.length());
  }
  return table;
}

void MonoExitTable::trace(JSTracer* trc) {
  for (uint32_t i = 0; i < numConstants_; i++) {
    TraceManuallyBarrieredEdge(trc, &constants()[i], "mono-exit-constant");
  }
}

void MonoExitTableWriter::writeSite(uint32_t stubOffset, uint32_t pcOffset,
                                    uint32_t drop, uint32_t numValues) {
  MOZ_ASSERT(stubOffset > lastStubOffset_);
  MOZ_ASSERT(drop <= 1);
  MOZ_RELEASE_ASSERT(numValues <= MonoExitMaxValues);
  sites_.writeUnsigned(stubOffset - lastStubOffset_);
  lastStubOffset_ = stubOffset;
  sites_.writeUnsigned(pcOffset);
  sites_.writeByte(drop);
  sites_.writeByte(numValues);
}

void MonoExitTableWriter::writeValue(MonoExitValue kind, uint32_t index) {
  MOZ_ASSERT(kind != MonoExitValue::Constant);
  sites_.writeByte(uint32_t(kind));
  if (kind != MonoExitValue::This) {
    sites_.writeUnsigned(index);
  }
}

bool MonoExitTableWriter::writeConstant(const Value& value) {
  sites_.writeByte(uint32_t(MonoExitValue::Constant));
  sites_.writeUnsigned(constants_.length());
  return constants_.append(value.asRawBits());
}

static void WriteOperandLocation(CompactBufferWriter& writer,
                                 const OperandLocation& loc) {
  writer.writeByte(uint32_t(loc.kind()));
  switch (loc.kind()) {
    case OperandLocation::ValueReg:
#ifdef JS_PUNBOX64
      writer.writeByte(loc.valueReg().valueReg().code());
      return;
#else
      break;
#endif
    case OperandLocation::PayloadReg:
      writer.writeByte(loc.payloadReg().code());
      writer.writeByte(uint32_t(loc.payloadType()));
      return;
    case OperandLocation::PayloadStack:
      writer.writeUnsigned(loc.payloadStack());
      writer.writeByte(uint32_t(loc.payloadType()));
      return;
    case OperandLocation::ValueStack:
      writer.writeUnsigned(loc.valueStack());
      return;
    case OperandLocation::BaselineFrame:
      writer.writeUnsigned(loc.baselineFrameSlot().slot());
      return;
    case OperandLocation::Uninitialized:
    case OperandLocation::DoubleReg:
    case OperandLocation::Constant:
      break;
  }
  MOZ_CRASH("Unexpected location of an inline stub input");
}

bool MonoExitTableWriter::encodeStates(
    MacroAssembler& masm, mozilla::Span<const OperandLocation> origInputs,
    mozilla::Span<FailurePath> failures, CompactBufferWriter& states,
    MonoExitJumpVector& jumps) {
#ifdef JS_CODEGEN_X64
  // The failure leaving the inputs in each state, but the first.
  Vector<const FailurePath*, 4, SystemAllocPolicy> stateFailures;
  Vector<uint32_t, 8, SystemAllocPolicy> stateOfFailure;
  if (!stateFailures.append(nullptr)) {
    return false;
  }
  for (const FailurePath& failure : failures) {
    uint32_t state = 0;
    if (failure.needsRestore(origInputs)) {
      state = 1;
      while (state < stateFailures.length() &&
             !stateFailures[state]->canShareFailurePath(failure)) {
        state++;
      }
      if (state == stateFailures.length() && !stateFailures.append(&failure)) {
        return false;
      }
    }
    if (!stateOfFailure.append(state)) {
      return false;
    }
  }

  MOZ_ASSERT(origInputs.size() <= MonoMaxStackInputs);
  states.writeByte(origInputs.size());
  states.writeByte(stateFailures.length());
  for (uint32_t state = 0; state < stateFailures.length(); state++) {
    size_t jumpsStart = jumps.length();
    bool ok = true;
    for (size_t i = 0; i < failures.size(); i++) {
      if (stateOfFailure[i] == state) {
        masm.forEachLabelUse(failures[i].labelUnchecked(),
                             [&](uint32_t offset) {
                               ok &= jumps.append(MonoExitJump{offset, state});
                             });
      }
    }
    if (!ok) {
      return false;
    }
    const FailurePath* failure = stateFailures[state];
    states.writeByte(jumps.length() != jumpsStart);
    states.writeUnsigned(failure ? failure->stackPushed() : 0);
    for (size_t i = 0; i < origInputs.size(); i++) {
      WriteOperandLocation(states, failure ? failure->input(i) : origInputs[i]);
    }
  }
  return !states.oom();
#else
  MOZ_CRASH("Monomorphic exit tables are x64 only");
#endif
}

void MonoExitTableWriter::writeStates(mozilla::Maybe<uint32_t> callsOffset,
                                      mozilla::Span<const uint8_t> states) {
  if (callsOffset) {
    MOZ_ASSERT(*callsOffset > lastCallsOffset_);
    sites_.writeUnsigned(*callsOffset - lastCallsOffset_);
    lastCallsOffset_ = *callsOffset;
  } else {
    sites_.writeUnsigned(0);
  }
  sites_.writeBytes(states.data(), states.size());
}

MonoExitTable* MonoExitTableWriter::finish() {
  if (oom()) {
    return nullptr;
  }
  return MonoExitTable::New(constants_, sites_);
}

namespace {

struct MonoExitInput {
  OperandLocation::Kind kind;
  uint32_t reg;
  uint32_t offset;
  JSValueType type;
};

struct MonoExitValueSource {
  MonoExitValue kind;
  uint32_t index;
};

// An exit decoded from the exit table.
struct MonoExit {
  uint32_t pcOffset;
  uint32_t drop;
  uint32_t numValues;
  MonoExitValueSource values[MonoExitMaxValues];
  uint32_t stackPushed;
  MonoExitInput inputs[MonoMaxStackInputs];
};

}  // namespace

static MonoExitInput ReadOperandLocation(CompactBufferReader& reader) {
  MonoExitInput input{};
  input.kind = OperandLocation::Kind(reader.readByte());
  switch (input.kind) {
    case OperandLocation::ValueReg:
      input.reg = reader.readByte();
      break;
    case OperandLocation::PayloadReg:
      input.reg = reader.readByte();
      input.type = JSValueType(reader.readByte());
      break;
    case OperandLocation::PayloadStack:
      input.offset = reader.readUnsigned();
      input.type = JSValueType(reader.readByte());
      break;
    case OperandLocation::ValueStack:
    case OperandLocation::BaselineFrame:
      input.offset = reader.readUnsigned();
      break;
    default:
      MOZ_CRASH("Unexpected location of an inline stub input");
  }
  return input;
}

// Finds the exit whose call returns to the code offset.
static MonoExit FindMonoExit(MonoExitTable* table, uint32_t returnOffset) {
  CompactBufferReader reader(table->dataStart(), table->dataEnd());
  uint32_t stubOffset = 0;
  uint32_t callsOffset = 0;
  while (reader.more()) {
    MonoExit exit;
    stubOffset += reader.readUnsigned();
    exit.pcOffset = reader.readUnsigned();
    exit.drop = reader.readByte();
    exit.numValues = reader.readByte();
    for (uint32_t i = 0; i < exit.numValues; i++) {
      MonoExitValueSource& value = exit.values[i];
      value.kind = MonoExitValue(reader.readByte());
      value.index =
          value.kind == MonoExitValue::This ? 0 : reader.readUnsigned();
    }
    callsOffset += reader.readUnsigned();
    uint32_t callReturnOffset = callsOffset;
    uint32_t numInputs = reader.readByte();
    uint32_t numStates = reader.readByte();
    MOZ_RELEASE_ASSERT(numInputs <= MonoMaxStackInputs);
    for (uint32_t state = 0; state < numStates; state++) {
      bool hasCall = reader.readByte();
      exit.stackPushed = reader.readUnsigned();
      for (uint32_t i = 0; i < numInputs; i++) {
        exit.inputs[i] = ReadOperandLocation(reader);
      }
      if (hasCall) {
        callReturnOffset += Assembler::PatchWrite_NearCallSize();
      }
      // The nop of a discarded stub calls the trampoline before the stub ran,
      // with its inputs where they are in the first state.
      bool discarded = state == 0 && returnOffset == stubOffset;
      if (discarded || (hasCall && returnOffset == callReturnOffset)) {
        return exit;
      }
    }
  }
  MOZ_CRASH("No monomorphic exit for the return address");
}

static Value BoxPayload(JSValueType type, uintptr_t payload) {
  switch (type) {
    case JSVAL_TYPE_INT32:
      return Int32Value(int32_t(payload));
    case JSVAL_TYPE_BOOLEAN:
      return BooleanValue(payload & 1);
    case JSVAL_TYPE_OBJECT:
      return ObjectValue(*reinterpret_cast<JSObject*>(payload));
    case JSVAL_TYPE_STRING:
      return StringValue(reinterpret_cast<JSString*>(payload));
    case JSVAL_TYPE_SYMBOL:
      return SymbolValue(reinterpret_cast<JS::Symbol*>(payload));
    case JSVAL_TYPE_BIGINT:
      return BigIntValue(reinterpret_cast<JS::BigInt*>(payload));
    default:
      MOZ_CRASH("Unexpected type of an unboxed input");
  }
}

static Value ReadMonoExitInput(const MonoExitInput& input, uintptr_t* regs,
                               uint8_t* stackTop) {
  switch (input.kind) {
    case OperandLocation::ValueReg:
      return Value::fromRawBits(regs[input.reg]);
    case OperandLocation::PayloadReg:
      return BoxPayload(input.type, regs[input.reg]);
    case OperandLocation::PayloadStack:
      return BoxPayload(input.type, *reinterpret_cast<uintptr_t*>(
                                        stackTop - input.offset));
    case OperandLocation::ValueStack:
      return *reinterpret_cast<Value*>(stackTop - input.offset);
    case OperandLocation::BaselineFrame:
      return reinterpret_cast<Value*>(stackTop)[input.offset];
    default:
      MOZ_CRASH("Unexpected location of an inline stub input");
  }
}

uint8_t* js::jit::MonoBailout(BaselineFrame* frame, uintptr_t* regs) {
#ifdef JS_CODEGEN_X64
  AutoUnsafeCallWithABI unsafe;
  BaselineScript* baselineScript = frame->script()->baselineScript();
  MOZ_ASSERT(baselineScript->monoExitTable());

  uint8_t* returnAddress = reinterpret_cast<uint8_t*>(regs[Registers::Total]);
  uint32_t returnOffset = returnAddress - baselineScript->method()->raw();
  MonoExit exit = FindMonoExit(baselineScript->monoExitTable(), returnOffset);

  // The stack as the compiler left it at the op, past what the stub pushed.
  uint8_t* stackTop = reinterpret_cast<uint8_t*>(regs + Registers::Total + 1) +
                      exit.stackPushed;

  Value values[MonoExitMaxValues];
  for (uint32_t i = 0; i < exit.numValues; i++) {
    const MonoExitValueSource& source = exit.values[i];
    switch (source.kind) {
      case MonoExitValue::Input:
        values[i] = ReadMonoExitInput(exit.inputs[source.index], regs,
                                      stackTop);
        break;
      case MonoExitValue::Local:
        values[i] = *reinterpret_cast<Value*>(
            reinterpret_cast<uint8_t*>(frame) + BaselineFrame::Size() +
            BaselineFrame::reverseOffsetOfLocal(source.index));
        break;
      case MonoExitValue::Arg:
        values[i] = frame->argv()[source.index];
        break;
      case MonoExitValue::This:
        values[i] = frame->thisArgument();
        break;
      case MonoExitValue::Register:
        values[i] = Value::fromRawBits(regs[source.index]);
        break;
      case MonoExitValue::Constant:
        values[i] = baselineScript->monoExitTable()->constant(source.index);
        break;
    }
  }

  Value* sp = reinterpret_cast<Value*>(stackTop) + exit.drop;
  for (uint32_t i = 0; i < exit.numValues; i++) {
    *--sp = values[i];
  }
  regs[StackPointer.code()] = uintptr_t(sp);

  return ContinueInInterpreter(frame, exit.pcOffset);
#else
  MOZ_CRASH("Monomorphic exit tables are x64 only");
#endif
}

static void LogDiscardBlocked(JSContext* cx, JSScript* script) {
  BaselineScript* baselineScript = script->baselineScript();
  uint32_t blocked = baselineScript->addMonoDiscardBlocked();
  if (!std::has_single_bit(blocked)) {
    return;
  }
  uint32_t monoFrames = 0;
  uint32_t jitFrames = 0;
  for (JitActivationIterator iter(cx); !iter.done(); ++iter) {
    for (OnlyJSJitFrameIter frames(iter); !frames.done(); ++frames) {
      const JSJitFrameIter& frame = frames.frame();
      if (!frame.isScripted()) {
        continue;
      }
      jitFrames++;
      if (frame.isBaselineJS() && frame.script() == script &&
          !frame.baselineFrame()->runningInInterpreter()) {
        monoFrames++;
      }
    }
  }
  LogMonoDiscardBlocked(script, baselineScript, blocked, monoFrames, jitFrames);
}

static uint32_t CountBaselineCodeFrames(JSContext* cx, JSScript* script) {
  uint32_t count = 0;
  for (JitActivationIterator iter(cx); !iter.done(); ++iter) {
    for (OnlyJSJitFrameIter frames(iter); !frames.done(); ++frames) {
      const JSJitFrameIter& frame = frames.frame();
      if (frame.isBaselineJS() && frame.script() == script &&
          !frame.baselineFrame()->runningInInterpreter()) {
        count++;
      }
    }
  }
  return count;
}

bool js::jit::MaybeDiscardFailedMonoBaseline(JSContext* cx, JSScript* script) {
  BaselineScript* baselineScript = script->baselineScript();
  MOZ_ASSERT(baselineScript->monoFailed());

  // No frame enters the code after it failed, so it is enough to count the
  // frames still running it once and have them count down as they leave.
  if (!baselineScript->monoFramesCounted()) {
    baselineScript->setMonoLiveFrames(CountBaselineCodeFrames(cx, script));
  }
  MOZ_ASSERT(baselineScript->monoLiveFrames() ==
             CountBaselineCodeFrames(cx, script));
  if (baselineScript->monoLiveFrames()) {
    if (baselineScript->monoStatsId()) {
      LogDiscardBlocked(cx, script);
    }
    return false;
  }
  if (script->hasIonScript()) {
    Invalidate(cx, script, /* resetUses = */ false);
  }
  CancelOffThreadIonCompile(script);
  if (baselineScript->monoStatsId()) {
    LogMonoScriptEvent("discard", script, baselineScript);
  }
  BaselineScript* cleared =
      script->jitScript()->clearBaselineScript(cx->gcContext(), script);
  MOZ_ASSERT(cleared == baselineScript);
  BaselineScript::Destroy(cx->gcContext(), cleared);
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

#ifdef JS_CODEGEN_X64
  // The exits of code with an exit table call this, with the return address
  // of the call telling the exit.
  monoBailoutOffset_ = startTrampolineCode(masm);
  masm.subFromStackPtr(Imm32(Registers::Total * sizeof(uintptr_t)));
  for (GeneralRegisterIterator iter(GeneralRegisterSet::All()); iter.more();
       ++iter) {
    Register reg = *iter;
    masm.storePtr(reg, Address(StackPointer, reg.code() * sizeof(uintptr_t)));
  }

  AllocatableGeneralRegisterSet bailoutRegs(GeneralRegisterSet::Volatile());
  Register bailoutFrame = bailoutRegs.takeAny();
  Register bailoutRegsPtr = bailoutRegs.takeAny();
  Register bailoutTemp = bailoutRegs.takeAny();
  masm.moveStackPtrTo(bailoutRegsPtr);
  masm.loadBaselineFramePtr(FramePointer, bailoutFrame);
  using BailoutFn = uint8_t* (*)(BaselineFrame*, uintptr_t*);
  masm.setupUnalignedABICall(bailoutTemp);
  masm.passABIArg(bailoutFrame);
  masm.passABIArg(bailoutRegsPtr);
  masm.callWithABI<BailoutFn, MonoBailout>();
  masm.loadStackPtr(
      Address(StackPointer, StackPointer.code() * sizeof(uintptr_t)));
  masm.jump(ReturnReg);
#endif
}
