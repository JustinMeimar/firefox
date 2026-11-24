/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MonoTemplates.h"

#include <algorithm>
#include <iterator>
#include <string.h>

#include "ds/LifoAlloc.h"
#include "gc/Barrier.h"
#include "gc/Zone.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/CompactBuffer.h"
#include "jit/JitContext.h"
#include "jit/JitZone.h"
#include "jit/MacroAssembler.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

bool js::jit::CanTemplateStubFieldsOf(CacheOp op) {
  switch (op) {
    case CacheOp::GuardShape:
    case CacheOp::GuardProto:
    case CacheOp::GuardSpecificObject:
    case CacheOp::GuardSpecificFunction:
    case CacheOp::GuardFunctionScript:
    case CacheOp::GuardSpecificSymbol:
    case CacheOp::GuardSpecificValue:
    case CacheOp::GuardHasProxyHandler:
    case CacheOp::GuardAnyClass:
    case CacheOp::GuardObjectFuseProperty:
    case CacheOp::GuardGlobalGeneration:
    case CacheOp::GuardFixedSlotValue:
    case CacheOp::GuardDynamicSlotValue:
    case CacheOp::GuardDynamicSlotIsSpecificObject:
    case CacheOp::GuardDynamicSlotIsNotObject:
    case CacheOp::GuardMultipleShapes:
    case CacheOp::CheckWeakValueResultForFixedSlot:
    case CacheOp::CheckWeakValueResultForDynamicSlot:
    case CacheOp::LoadObject:
    case CacheOp::LoadProtoObject:
    case CacheOp::LoadInt32Constant:
    case CacheOp::LoadConstantString:
    case CacheOp::LoadFixedSlot:
    case CacheOp::LoadDynamicSlot:
    case CacheOp::LoadFixedSlotResult:
    case CacheOp::LoadDynamicSlotResult:
    case CacheOp::LoadValueResult:
    case CacheOp::LoadConstantStringResult:
    case CacheOp::UncheckedLoadWeakValueResult:
    case CacheOp::UncheckedLoadWeakObjectResult:
    case CacheOp::StoreFixedSlot:
    case CacheOp::StoreDynamicSlot:
    case CacheOp::StoreFixedSlotUndefinedResult:
    case CacheOp::AssertPropertyLookup:
      return true;
    default:
      return false;
  }
}

namespace {

// How a template holds a stub field.
enum class FieldUse : uint8_t {
  // The bits of the field are part of the key.
  Key,
  Int32,
  Word,
  GCThing,
};

struct TemplateField {
  uint32_t offset;
  StubField::Type type;
  FieldUse use;
};

using TemplateFieldVector = Vector<TemplateField, 8, SystemAllocPolicy>;
using TemplateKey = Vector<uint8_t, 256, SystemAllocPolicy>;

}  // namespace

static bool IsValueField(StubField::Type type) {
  return type == StubField::Type::Value || type == StubField::Type::WeakValue;
}

// The type of the Values the code may box a GC thing field as.
static JSValueType BoxedTypeOf(StubField::Type type) {
  switch (type) {
    case StubField::Type::JSObject:
    case StubField::Type::WeakObject:
      return JSVAL_TYPE_OBJECT;
    case StubField::Type::String:
      return JSVAL_TYPE_STRING;
    case StubField::Type::Symbol:
      return JSVAL_TYPE_SYMBOL;
    default:
      return JSVAL_TYPE_UNKNOWN;
  }
}

// Returns false if no template can hold the field.
static bool ClassifyField(StubField::Type type, const uint8_t* field,
                          FieldUse* use) {
  uintptr_t word = *reinterpret_cast<const uintptr_t*>(field);
  switch (type) {
    case StubField::Type::RawInt32:
      *use = FieldUse::Int32;
      return true;
    case StubField::Type::RawPointer:
    case StubField::Type::AllocSite:
    case StubField::Type::RawInt64:
      *use = FieldUse::Word;
      return true;
    case StubField::Type::Shape:
    case StubField::Type::WeakShape:
    case StubField::Type::JSObject:
    case StubField::Type::WeakObject:
    case StubField::Type::Symbol:
    case StubField::Type::String:
    case StubField::Type::WeakBaseScript:
      if (!word) {
        return false;
      }
      MOZ_ASSERT(!gc::IsInsideNursery(reinterpret_cast<gc::Cell*>(word)));
      *use = FieldUse::GCThing;
      return true;
    case StubField::Type::Value:
    case StubField::Type::WeakValue: {
      Value v = Value::fromRawBits(*reinterpret_cast<const uint64_t*>(field));
      if (v.isGCThing()) {
        MOZ_ASSERT(!gc::IsInsideNursery(v.toGCThing()));
        *use = FieldUse::GCThing;
      } else if (v.isInt32()) {
        *use = FieldUse::Word;
      } else {
        *use = FieldUse::Key;
      }
      return true;
    }
    case StubField::Type::Double:
      *use = FieldUse::Key;
      return true;
    case StubField::Type::Id: {
      jsid id = jsid::fromRawBits(word);
      if (id.isSymbol()) {
        return false;
      }
      *use = id.isString() ? FieldUse::GCThing : FieldUse::Key;
      return true;
    }
    case StubField::Type::ICScript:
    case StubField::Type::JitCode:
    case StubField::Type::Limit:
      break;
  }
  return false;
}

template <typename T>
static bool AppendBytes(TemplateKey& key, const T& value) {
  return key.append(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

using TemplateKeySuffix = Vector<uint8_t, 64, SystemAllocPolicy>;

// Builds the end of the key of the stub, in which stubs of the same stub info
// differ, and the uses of its fields. Returns false on OOM, and true with
// *supported false if no template can hold its fields.
static bool BuildKeySuffix(ICCacheIRStub* stub,
                           mozilla::Span<const JSValueType> inputTypes,
                           TemplateFieldVector& fields,
                           TemplateKeySuffix& suffix, bool* supported) {
  *supported = false;
  const CacheIRStubInfo* info = stub->stubInfo();
  const uint8_t* data = stub->stubDataStart();

  size_t offset = 0;
  for (uint32_t i = 0;; i++) {
    StubField::Type type = info->fieldType(i);
    if (type == StubField::Type::Limit) {
      break;
    }
    FieldUse use;
    if (!ClassifyField(type, data + offset, &use)) {
      return true;
    }
    if (!fields.append(TemplateField{uint32_t(offset), type, use})) {
      return false;
    }
    offset += StubField::sizeInBytes(type);
  }
  *supported = true;

  if (!suffix.append(uint8_t(inputTypes.size()))) {
    return false;
  }
  for (JSValueType type : inputTypes) {
    if (!suffix.append(uint8_t(type))) {
      return false;
    }
  }

  for (const TemplateField& field : fields) {
    if (!suffix.append(uint8_t(field.use))) {
      return false;
    }
    const uint8_t* bits = data + field.offset;
    if (IsValueField(field.type)) {
      Value v = Value::fromRawBits(*reinterpret_cast<const uint64_t*>(bits));
      if (!suffix.append(uint8_t(v.type()))) {
        return false;
      }
    }
    if (field.use == FieldUse::Key &&
        !suffix.append(bits, StubField::sizeInBytes(field.type))) {
      return false;
    }
  }
  return true;
}

// Builds the key of a stub of the info: the CacheIR, with its kind, operand
// counts and last uses, code and field types, then the end of the key. Returns
// false on OOM.
static bool BuildKey(const CacheIRStubInfo* info, size_t numFields,
                     const TemplateKeySuffix& suffix, TemplateKey& key) {
  const uint8_t* start =
      reinterpret_cast<const uint8_t*>(info->operandLastUsed());
  const uint8_t* end = info->code() + info->codeLength() + numFields + 1;
  return key.append(uint8_t(info->kind())) &&
         key.append(uint8_t(info->numInputOperands())) &&
         key.append(uint8_t(info->numOperandIds())) &&
         key.append(uint8_t(info->numOperandLastUsed())) &&
         AppendBytes(key, info->codeLength()) && key.append(start, end) &&
         key.appendAll(suffix);
}

static const MonoStubTemplate* FindRecentTemplate(
    const CacheIRStubInfo::MonoBaselineCache& cache,
    const TemplateKeySuffix& suffix) {
  for (const MonoStubTemplate* templ : cache.recentTemplates) {
    if (!templ) {
      break;
    }
    mozilla::Span<const uint8_t> templSuffix = templ->keySuffix();
    if (templSuffix.size() == suffix.length() &&
        memcmp(templSuffix.data(), suffix.begin(), suffix.length()) == 0) {
      return templ;
    }
  }
  return nullptr;
}

static void AddRecentTemplate(CacheIRStubInfo::MonoBaselineCache& cache,
                              const MonoStubTemplate* templ) {
  for (size_t i = std::size(cache.recentTemplates) - 1; i > 0; i--) {
    cache.recentTemplates[i] = cache.recentTemplates[i - 1];
  }
  cache.recentTemplates[0] = templ;
}

bool MonoStubTemplates::Hasher::match(const UniquePtr<MonoStubTemplate>& entry,
                                      const Lookup& lookup) {
  mozilla::Span<const uint8_t> key = entry->key();
  return entry->hash() == lookup.hash && key.size() == lookup.key.size() &&
         memcmp(key.data(), lookup.key.data(), key.size()) == 0;
}

static void ReadBarrierWeakFields(const TemplateFieldVector& fields,
                                  const uint8_t* data) {
  for (const TemplateField& field : fields) {
    const uint8_t* bits = data + field.offset;
    uintptr_t word = *reinterpret_cast<const uintptr_t*>(bits);
    switch (field.type) {
      case StubField::Type::WeakShape:
        gc::ReadBarrier(reinterpret_cast<Shape*>(word));
        break;
      case StubField::Type::WeakObject:
        gc::ReadBarrier(reinterpret_cast<JSObject*>(word));
        break;
      case StubField::Type::WeakBaseScript:
        gc::ReadBarrier(reinterpret_cast<BaseScript*>(word));
        break;
      case StubField::Type::WeakValue:
        gc::ValueReadBarrier(
            Value::fromRawBits(*reinterpret_cast<const uint64_t*>(bits)));
        break;
      default:
        break;
    }
  }
}

bool MonoStubTemplate::instantiate(MacroAssembler& masm, JS::Zone* zone,
                                   const uint8_t* stubData,
                                   InlinedInputVector& origInputs,
                                   InlinedFailureVector& failures) const {
#ifdef JS_CODEGEN_X64
  MOZ_ASSERT(!rejected_);
  uint32_t base = masm.currentOffset();
  if (!masm.appendRawCode(code_.begin(), code_.length())) {
    return false;
  }

  for (const Hole& hole : holes_) {
    uint32_t offset = base + hole.offset;
    switch (hole.kind) {
      case HoleKind::Int32Field:
        masm.patchRawCode(offset, stubData + hole.fieldOffset,
                          sizeof(int32_t));
        break;
      case HoleKind::WordField:
        masm.patchRawCode(offset, stubData + hole.fieldOffset,
                          sizeof(uint64_t));
        break;
      case HoleKind::GCThingField:
        masm.patchRawCode(offset, stubData + hole.fieldOffset,
                          sizeof(uint64_t));
        masm.addDataRelocationAt(offset + sizeof(uint64_t));
        break;
      case HoleKind::BoxedGCThingField: {
        uint64_t bits =
            JS::detail::ValueTypeToShiftedTag(hole.boxedType) |
            *reinterpret_cast<const uintptr_t*>(stubData + hole.fieldOffset);
        masm.patchRawCode(offset, &bits, sizeof(bits));
        masm.addDataRelocationAt(offset + sizeof(uint64_t));
        break;
      }
      case HoleKind::ZoneNeedsMarkingBarrier: {
        const void* flag = zone->addressOfNeedsMarkingBarrier();
        masm.patchRawCode(offset, &flag, sizeof(flag));
        break;
      }
    }
  }

  for (const PendingJump& jump : jumps_) {
    masm.addPendingJumpAt(base + jump.offset, jump.target, jump.kind);
  }

  uint32_t jumpsStart = 0;
  for (size_t i = 0; i < failures_.length(); i++) {
    const FailurePath& templateFailure = failures_[i];
    FailurePath failure;
    for (size_t j = 0; j < origInputs_.length(); j++) {
      if (!failure.appendInput(templateFailure.input(j))) {
        return false;
      }
    }
    if (!failure.setSpilledRegs(templateFailure.spilledRegs())) {
      return false;
    }
    failure.setStackPushed(templateFailure.stackPushed());
    for (uint32_t j = jumpsStart; j < failureJumpsEnd_[i]; j++) {
      masm.useLabelAt(failure.labelUnchecked(), base + failureJumps_[j]);
    }
    jumpsStart = failureJumpsEnd_[i];
    if (!failures.append(std::move(failure))) {
      return false;
    }
  }

  return origInputs.appendAll(origInputs_) && !masm.oom();
#else
  MOZ_CRASH("Monomorphic Baseline templates are x64 only");
#endif
}

#ifdef JS_CODEGEN_X64

static constexpr size_t NumTemplateCompiles = 3;

// The sentinel replacing the field in the given compile, with the number of
// its bytes the code holds in *width.
static uint64_t Sentinel(const TemplateField& field, size_t index,
                         size_t compile, const uint8_t* stubData,
                         size_t* width) {
  uint64_t bits = *reinterpret_cast<const uint64_t*>(stubData + field.offset);
  uint32_t int32 = 0x11111110u * uint32_t(compile + 1) + 16 * uint32_t(index);
  switch (field.use) {
    case FieldUse::Int32:
      *width = sizeof(int32_t);
      return int32;
    case FieldUse::Word:
      *width = sizeof(uint64_t);
      if (IsValueField(field.type)) {
        return Int32Value(int32_t(int32)).asRawBits();
      }
      return 0x00003A3A3A3A0000 + 0x0000111111110000 * compile + 16 * index;
    case FieldUse::GCThing: {
      *width = sizeof(uint64_t);
      uint64_t payload = bits;
      if (IsValueField(field.type)) {
        payload &= JS::detail::ValueGCThingPayloadMask;
      }
      uint64_t arena = payload & ~uint64_t(gc::ArenaMask);
      uint64_t cell =
          arena + ((16 * (1 + index * NumTemplateCompiles + compile)) &
                   gc::ArenaMask);
      if (IsValueField(field.type)) {
        return (bits & ~JS::detail::ValueGCThingPayloadMask) | cell;
      }
      return cell;
    }
    case FieldUse::Key:
      break;
  }
  MOZ_CRASH("Unexpected field use");
}

class MOZ_RAII js::jit::MonoStubTemplateBuilder {
  JSContext* cx_;
  ICCacheIRStub* stub_;
  mozilla::Span<const JSValueType> inputTypes_;
  const TemplateFieldVector& fields_;
  MonoStubTemplate& template_;

  struct Compile {
    MacroAssembler* masm;
    Vector<uint8_t, 64, SystemAllocPolicy> stubData;
    InlinedInputVector origInputs;
    InlinedFailureVector failures;
    Vector<uint32_t, 8, SystemAllocPolicy> relocations;
    Vector<MonoStubTemplate::PendingJump, 4, SystemAllocPolicy> jumps;
    Vector<uint32_t, 8, SystemAllocPolicy> failureJumps;
    Vector<uint32_t, 4, SystemAllocPolicy> failureJumpsEnd;
  };
  Compile compiles_[NumTemplateCompiles];

  // The hole covering each byte of the code, if any.
  Vector<bool, 256, SystemAllocPolicy> inHole_;

  [[nodiscard]] bool compile(size_t index, TempAllocator& alloc,
                             bool* rejected);
  [[nodiscard]] bool findHoles(const uint64_t* sentinels, size_t width,
                               const MonoStubTemplate::Hole& hole,
                               bool* rejected);
  [[nodiscard]] bool findFieldHoles(bool* rejected);
  [[nodiscard]] bool findZoneHoles(bool* rejected);
  bool pointsIntoContext(uint64_t word) const;

 public:
  MonoStubTemplateBuilder(JSContext* cx, ICCacheIRStub* stub,
                          mozilla::Span<const JSValueType> inputTypes,
                          const TemplateFieldVector& fields,
                          MonoStubTemplate& templ)
      : cx_(cx),
        stub_(stub),
        inputTypes_(inputTypes),
        fields_(fields),
        template_(templ) {}

  // Builds the template, or returns true with *rejected set if no template
  // can be built. Returns false on OOM.
  [[nodiscard]] bool build(bool* rejected);
};

bool MonoStubTemplateBuilder::compile(size_t index, TempAllocator& alloc,
                                      bool* rejected) {
  Compile& compile = compiles_[index];
  const CacheIRStubInfo* info = stub_->stubInfo();
  const uint8_t* stubData = stub_->stubDataStart();

  if (!compile.stubData.append(stubData, info->stubDataSize())) {
    return false;
  }
  for (size_t i = 0; i < fields_.length(); i++) {
    const TemplateField& field = fields_[i];
    if (field.use == FieldUse::Key) {
      continue;
    }
    size_t width;
    uint64_t sentinel = Sentinel(field, i, index, stubData, &width);
    memcpy(compile.stubData.begin() + field.offset, &sentinel, width);
  }

  MacroAssembler& masm = *compile.masm;
  BaselineCacheIRCompiler compiler(
      cx_, alloc, CacheIRSource(info, compile.stubData.begin()), 0, &masm);
  compiler.setTemplateStubData(stubData);
  if (!compiler.init(info->kind()) ||
      !compiler.compileInline(inputTypes_, compile.origInputs,
                              compile.failures)) {
    return false;
  }
  if (masm.oom()) {
    return false;
  }
  if (compiler.templateRejected() || masm.hasConstantPoolEntries() ||
      masm.numCodeLabels() != 0 || masm.embedsNurseryPointers()) {
    *rejected = true;
    return true;
  }

  if (masm.dataRelocations().length() != 0) {
    CompactBufferReader reader(masm.dataRelocations());
    while (reader.more()) {
      if (!compile.relocations.append(reader.readUnsigned())) {
        return false;
      }
    }
  }
  std::sort(compile.relocations.begin(), compile.relocations.end());

  bool ok = true;
  masm.forEachPendingJump([&](uint32_t offset, void* target,
                              RelocationKind kind) {
    ok &= compile.jumps.append(
        MonoStubTemplate::PendingJump{offset, target, kind});
  });
  if (!ok) {
    return false;
  }

  for (FailurePath& failure : compile.failures) {
    masm.forEachLabelUse(failure.labelUnchecked(), [&](uint32_t offset) {
      ok &= compile.failureJumps.append(offset);
    });
    ok &= compile.failureJumpsEnd.append(compile.failureJumps.length());
  }
  return ok;
}

bool MonoStubTemplateBuilder::findHoles(const uint64_t* sentinels, size_t width,
                                        const MonoStubTemplate::Hole& hole,
                                        bool* rejected) {
  size_t length = compiles_[0].masm->size();
  for (size_t offset = 0; offset + width <= length; offset++) {
    if (memcmp(compiles_[0].masm->rawCode() + offset, &sentinels[0], width) !=
        0) {
      continue;
    }
    for (size_t j = 1; j < NumTemplateCompiles; j++) {
      if (memcmp(compiles_[j].masm->rawCode() + offset, &sentinels[j],
                 width) != 0) {
        *rejected = true;
        return true;
      }
    }
    for (size_t j = offset; j < offset + width; j++) {
      if (inHole_[j]) {
        *rejected = true;
        return true;
      }
      inHole_[j] = true;
    }
    MonoStubTemplate::Hole found = hole;
    found.offset = offset;
    if (!template_.holes_.append(found)) {
      return false;
    }
  }
  return true;
}

bool MonoStubTemplateBuilder::findFieldHoles(bool* rejected) {
  size_t length = compiles_[0].masm->size();
  const uint8_t* code[NumTemplateCompiles];
  for (size_t i = 0; i < NumTemplateCompiles; i++) {
    code[i] = compiles_[i].masm->rawCode();
  }

  const uint8_t* stubData = stub_->stubDataStart();
  for (size_t i = 0; i < fields_.length(); i++) {
    const TemplateField& field = fields_[i];
    MonoStubTemplate::HoleKind kind;
    switch (field.use) {
      case FieldUse::Key:
        continue;
      case FieldUse::Int32:
        kind = MonoStubTemplate::HoleKind::Int32Field;
        break;
      case FieldUse::Word:
        kind = MonoStubTemplate::HoleKind::WordField;
        break;
      case FieldUse::GCThing:
        kind = MonoStubTemplate::HoleKind::GCThingField;
        break;
    }

    uint64_t sentinels[NumTemplateCompiles];
    size_t width;
    for (size_t j = 0; j < NumTemplateCompiles; j++) {
      sentinels[j] = Sentinel(field, i, j, stubData, &width);
    }
    MonoStubTemplate::Hole hole{0, field.offset, kind, JSVAL_TYPE_UNKNOWN};
    if (!findHoles(sentinels, width, hole, rejected)) {
      return false;
    }
    if (*rejected) {
      return true;
    }

    JSValueType boxedType = BoxedTypeOf(field.type);
    if (boxedType != JSVAL_TYPE_UNKNOWN) {
      for (uint64_t& sentinel : sentinels) {
        sentinel |= JS::detail::ValueTypeToShiftedTag(boxedType);
      }
      hole.kind = MonoStubTemplate::HoleKind::BoxedGCThingField;
      hole.boxedType = boxedType;
      if (!findHoles(sentinels, width, hole, rejected)) {
        return false;
      }
      if (*rejected) {
        return true;
      }
    }
  }

  for (size_t offset = 0; offset < length; offset++) {
    if (inHole_[offset]) {
      continue;
    }
    for (size_t j = 1; j < NumTemplateCompiles; j++) {
      if (code[j][offset] != code[0][offset]) {
        *rejected = true;
        return true;
      }
    }
  }
  return true;
}

bool MonoStubTemplateBuilder::pointsIntoContext(uint64_t word) const {
  auto inside = [word](const void* start, size_t size) {
    return word >= uintptr_t(start) && word < uintptr_t(start) + size;
  };
  JS::Zone* zone = cx_->zone();
  return inside(zone, sizeof(JS::Zone)) ||
         inside(cx_->realm(), sizeof(JS::Realm)) ||
         (zone->jitZone() && inside(zone->jitZone(), sizeof(JitZone)));
}

bool MonoStubTemplateBuilder::findZoneHoles(bool* rejected) {
  size_t length = compiles_[0].masm->size();
  const uint8_t* code = compiles_[0].masm->rawCode();
  uint64_t flag = uintptr_t(cx_->zone()->addressOfNeedsMarkingBarrier());
  for (size_t offset = 0; offset + sizeof(uint64_t) <= length; offset++) {
    bool covered = false;
    for (size_t j = offset; j < offset + sizeof(uint64_t); j++) {
      covered |= inHole_[j];
    }
    if (covered) {
      continue;
    }
    uint64_t word;
    memcpy(&word, code + offset, sizeof(word));
    if (word == flag) {
      for (size_t j = offset; j < offset + sizeof(uint64_t); j++) {
        inHole_[j] = true;
      }
      if (!template_.holes_.append(MonoStubTemplate::Hole{
              uint32_t(offset), 0,
              MonoStubTemplate::HoleKind::ZoneNeedsMarkingBarrier,
              JSVAL_TYPE_UNKNOWN})) {
        return false;
      }
    } else if (pointsIntoContext(word)) {
      *rejected = true;
      return true;
    }
  }
  return true;
}

bool MonoStubTemplateBuilder::build(bool* rejected) {
  *rejected = false;

  LifoAlloc lifo(TempAllocator::PreferredLifoChunkSize, js::MallocArena);
  TempAllocator alloc(&lifo);
  StackMacroAssembler masm0(cx_, alloc);
  StackMacroAssembler masm1(cx_, alloc);
  StackMacroAssembler masm2(cx_, alloc);
  MacroAssembler* masms[NumTemplateCompiles] = {&masm0, &masm1, &masm2};
  for (size_t i = 0; i < NumTemplateCompiles; i++) {
    compiles_[i].masm = masms[i];
    if (!compile(i, alloc, rejected)) {
      return false;
    }
    if (*rejected) {
      return true;
    }
  }

  const Compile& first = compiles_[0];
  size_t length = first.masm->size();
  for (size_t i = 1; i < NumTemplateCompiles; i++) {
    const Compile& other = compiles_[i];
    bool same = other.masm->size() == length &&
                other.origInputs.length() == first.origInputs.length() &&
                other.failures.length() == first.failures.length() &&
                other.relocations.length() == first.relocations.length() &&
                other.jumps.length() == first.jumps.length() &&
                other.failureJumps.length() == first.failureJumps.length() &&
                other.failureJumpsEnd.length() ==
                    first.failureJumpsEnd.length();
    for (size_t j = 0; same && j < first.origInputs.length(); j++) {
      same = other.origInputs[j] == first.origInputs[j];
    }
    for (size_t j = 0; same && j < first.failures.length(); j++) {
      same = other.failures[j].canShareFailurePath(first.failures[j]);
    }
    for (size_t j = 0; same && j < first.relocations.length(); j++) {
      same = other.relocations[j] == first.relocations[j];
    }
    for (size_t j = 0; same && j < first.jumps.length(); j++) {
      same = other.jumps[j].offset == first.jumps[j].offset &&
             other.jumps[j].target == first.jumps[j].target &&
             other.jumps[j].kind == first.jumps[j].kind;
    }
    for (size_t j = 0; same && j < first.failureJumps.length(); j++) {
      same = other.failureJumps[j] == first.failureJumps[j];
    }
    for (size_t j = 0; same && j < first.failureJumpsEnd.length(); j++) {
      same = other.failureJumpsEnd[j] == first.failureJumpsEnd[j];
    }
    if (!same) {
      *rejected = true;
      return true;
    }
  }

  if (!inHole_.appendN(false, length)) {
    return false;
  }
  if (!findFieldHoles(rejected)) {
    return false;
  }
  if (*rejected) {
    return true;
  }
  if (!findZoneHoles(rejected)) {
    return false;
  }
  if (*rejected) {
    return true;
  }

  // Every GC thing in the code is a hole, so that its instances relocate the
  // stub's things.
  Vector<uint32_t, 8, SystemAllocPolicy> gcThingEnds;
  for (const MonoStubTemplate::Hole& hole : template_.holes_) {
    bool gcThing =
        hole.kind == MonoStubTemplate::HoleKind::GCThingField ||
        hole.kind == MonoStubTemplate::HoleKind::BoxedGCThingField;
    if (gcThing && !gcThingEnds.append(hole.offset + sizeof(uint64_t))) {
      return false;
    }
  }
  std::sort(gcThingEnds.begin(), gcThingEnds.end());
  if (gcThingEnds.length() != first.relocations.length() ||
      !std::equal(gcThingEnds.begin(), gcThingEnds.end(),
                  first.relocations.begin())) {
    *rejected = true;
    return true;
  }

  // Jumps are relative to the code, so their targets can't be in holes.
  auto overlapsHole = [&](uint32_t end) {
    for (uint32_t j = end - sizeof(int32_t); j < end; j++) {
      if (inHole_[j]) {
        return true;
      }
    }
    return false;
  };
  for (const MonoStubTemplate::PendingJump& jump : first.jumps) {
    if (overlapsHole(jump.offset)) {
      *rejected = true;
      return true;
    }
  }
  for (uint32_t offset : first.failureJumps) {
    if (overlapsHole(offset)) {
      *rejected = true;
      return true;
    }
  }

  Compile& result = compiles_[0];
  template_.origInputs_ = std::move(result.origInputs);
  template_.failures_ = std::move(result.failures);
  return template_.code_.append(result.masm->rawCode(), length) &&
         template_.jumps_.appendAll(result.jumps) &&
         template_.failureJumps_.appendAll(result.failureJumps) &&
         template_.failureJumpsEnd_.appendAll(result.failureJumpsEnd);
}

#endif  // JS_CODEGEN_X64

bool MonoStubTemplates::emitStub(JSContext* cx, MacroAssembler& masm,
                                 ICCacheIRStub* stub,
                                 mozilla::Span<const JSValueType> inputTypes,
                                 InlinedInputVector& origInputs,
                                 InlinedFailureVector& failures,
                                 MonoTemplateUse* use) {
  *use = MonoTemplateUse::None;
#ifdef JS_CODEGEN_X64
  const CacheIRStubInfo* info = stub->stubInfo();
  TemplateFieldVector fields;
  TemplateKeySuffix suffix;
  bool supported;
  if (!BuildKeySuffix(stub, inputTypes, fields, suffix, &supported)) {
    return false;
  }
  if (!supported) {
    return true;
  }

  CacheIRStubInfo::MonoBaselineCache& cache = info->monoBaselineCache();
  const MonoStubTemplate* found = FindRecentTemplate(cache, suffix);
  if (found) {
    *use = MonoTemplateUse::Hit;
  } else {
    TemplateKey key;
    if (!BuildKey(info, fields.length(), suffix, key)) {
      return false;
    }
    HashNumber hash = mozilla::HashBytes(key.begin(), key.length());
    Hasher::Lookup lookup{
        mozilla::Span<const uint8_t>(key.begin(), key.length()), hash};
    auto p = templates_.lookupForAdd(lookup);
    if (p) {
      *use = MonoTemplateUse::Hit;
    } else {
      auto templ = MakeUnique<MonoStubTemplate>(hash);
      if (!templ || !templ->key_.appendAll(key)) {
        return false;
      }
      templ->keySuffixLength_ = suffix.length();
      bool rejected;
      MonoStubTemplateBuilder builder(cx, stub, inputTypes, fields, *templ);
      if (!builder.build(&rejected)) {
        return false;
      }
      if (rejected) {
        templ->rejected_ = true;
        templ->holes_.clearAndFree();
      }
      if (!templates_.add(p, std::move(templ))) {
        return false;
      }
      *use = MonoTemplateUse::Built;
    }
    found = p->get();
    AddRecentTemplate(cache, found);
  }

  const MonoStubTemplate& templ = *found;
  if (templ.rejected()) {
    *use = MonoTemplateUse::Rejected;
    return true;
  }

  const uint8_t* stubData = stub->stubDataStart();
  ReadBarrierWeakFields(fields, stubData);
  return templ.instantiate(masm, cx->zone(), stubData, origInputs, failures);
#else
  return true;
#endif
}
