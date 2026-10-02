/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/SpecializationTrace.h"

#include "mozilla/Atomics.h"

#include <algorithm>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jit/BaselineIC.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRReader.h"
#include "jit/IonScript.h"
#include "jit/JitScript.h"
#include "jit/MIRGenerator.h"
#include "jit/WarpSnapshot.h"
#include "js/Printer.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "util/GetPidProvider.h"
#include "vm/JSFunction.h"
#include "vm/JSONPrinter.h"
#include "vm/JSScript.h"
#include "vm/Logging.h"
#include "vm/MutexIDs.h"
#include "vm/Realm.h"
#include "vm/Shape.h"
#include "vm/Time.h"

#include "vm/JSScript-inl.h"
#include "vm/Shape-inl.h"

namespace js::jit {

static mozilla::Atomic<uint64_t, mozilla::Relaxed> specializationSequence(0);

static uint64_t NextSpecializationId() { return ++specializationSequence; }

static int64_t SpecializationProcessStart() {
  static const int64_t start = PRMJ_Now();
  return start;
}

static const char* SpecializationBuildId() {
  const char* id = getenv("JS_SPECIALIZATION_BUILD_ID");
  return id ? id : "unspecified";
}

static Mutex& SpecializationOutputMutex() {
  static Mutex mutex(mutexid::SpecializationTrace);
  return mutex;
}

template <typename F>
static void SpecializationEvent(const char* event, F&& write) {
  if (!JS_SHOULD_LOG(specializationCache, Debug)) {
    return;
  }
  Sprinter buffer(nullptr, false);
  if (!buffer.init()) {
    JS_LOG(specializationCache, Debug, "IONFEEDBACK\tlost");
    return;
  }
  JSONPrinter json(buffer, false);
  json.beginObject();
  json.property("event", event);
  json.property("pid", int32_t(getpid()));
  json.property("process_start_us", SpecializationProcessStart());
  json.property("timestamp_us", int64_t(PRMJ_Now()));
  uint64_t sequence = NextSpecializationId();
  json.property("sequence", sequence);
  json.property("build_id", SpecializationBuildId());
  json.property("pointer_bytes", uint32_t(sizeof(uintptr_t)));
  write(json);
  json.endObject();
  if (buffer.hadOutOfMemory()) {
    JS_LOG(specializationCache, Debug, "IONFEEDBACK\tlost");
    return;
  }
  auto chars = buffer.release();
  // Forked content processes can inherit the same MOZ_LOG file. Keep each
  // synchronous log write below the stdio buffer size and reassemble by ID.
  constexpr size_t chunkBytes = 512;
  size_t length = strlen(chars.get());
  size_t chunks = (length + chunkBytes - 1) / chunkBytes;
  LockGuard<Mutex> lock(SpecializationOutputMutex());
  for (size_t part = 0; part < chunks; part++) {
    char hex[chunkBytes * 2 + 1];
    size_t count = std::min(chunkBytes, length - part * chunkBytes);
    for (size_t i = 0; i < count; i++) {
      uint8_t byte = uint8_t(chars.get()[part * chunkBytes + i]);
      hex[2 * i] = "0123456789abcdef"[byte >> 4];
      hex[2 * i + 1] = "0123456789abcdef"[byte & 15];
    }
    hex[2 * count] = 0;
    JS_LOG(specializationCache, Debug,
           "IONFEEDBACK\t%d\t%" PRId64 "\t%" PRIu64 "\t%zu\t%zu\t%s",
           int(getpid()), SpecializationProcessStart(), sequence, part, chunks,
           hex);
  }
}

static uint64_t SpecializationHash(const void* bytes, size_t length,
                                   uint64_t hash = 14695981039346656037ULL) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < length; i++) {
    hash = (hash ^ p[i]) * 1099511628211ULL;
  }
  return hash;
}

static void HexProperty(JSONPrinter& json, const char* name,
                        const uint8_t* bytes, size_t length) {
  GenericPrinter& out = json.beginStringProperty(name);
  for (size_t i = 0; i < length; i++) {
    out.printf("%02x", bytes[i]);
  }
  json.endStringProperty();
}

template <typename CharT>
static void StringProperty(JSONPrinter& json, const char* name,
                           const CharT* chars, size_t length,
                           bool escapeNonAscii = true) {
  GenericPrinter& out = json.beginStringProperty(name);
  for (size_t i = 0; i < length; i++) {
    uint32_t c = chars[i];
    if (c == '"' || c == '\\') {
      out.putChar('\\');
      out.putChar(char(c));
    } else if (c < 0x20 || (escapeNonAscii && c > 0x7e)) {
      out.printf("\\u%04x", c);
    } else {
      out.putChar(char(c));
    }
  }
  json.endStringProperty();
}

struct SpecializationIdentity {
  uint64_t sourceHash = 0;
  uint64_t bytecodeHash = 0;
  uint64_t key = 0;
  uint32_t encoding = 0;
  const char* kind = "unresolved";

  explicit SpecializationIdentity(JSScript* script) {
    bytecodeHash = SpecializationHash(script->code(), script->length());
    ScriptSource* source = script->scriptSource();
    if (script->selfHosted()) {
      uint64_t inputs[] = {bytecodeHash, script->sourceStart(),
                           script->sourceEnd(),
                           script->immutableFlags().toRaw()};
      key = SpecializationHash(inputs, sizeof(inputs));
      kind = "self_hosted_build_coordinates";
      return;
    }
    if (!source->hasSourceText() || script->sourceEnd() > source->length()) {
      return;
    }
    size_t length = script->sourceLength();
    if (!length) {
      sourceHash = SpecializationHash(nullptr, 0);
      encoding = source->hasSourceType<char16_t>() ? 16 : 8;
    } else {
      auto chars =
          source->substringChars(script->sourceStart(), script->sourceEnd());
      if (chars.is<JS::UniqueChars>()) {
        if (!chars.as<JS::UniqueChars>()) {
          return;
        }
        sourceHash =
            SpecializationHash(chars.as<JS::UniqueChars>().get(), length);
        encoding = 8;
      } else {
        if (!chars.as<JS::UniqueTwoByteChars>()) {
          return;
        }
        sourceHash =
            SpecializationHash(chars.as<JS::UniqueTwoByteChars>().get(),
                               length * sizeof(char16_t));
        encoding = 16;
      }
    }
    uint64_t inputs[] = {sourceHash, bytecodeHash, encoding,
                         script->immutableFlags().toRaw()};
    key = SpecializationHash(inputs, sizeof(inputs));
    kind = "source_bytecode";
  }
};

void InitSpecializationTrace(JSScript* script, ICScript* icScript) {
  if (!JS_SHOULD_LOG(specializationCache, Debug) ||
      icScript->specializationTraceId()) {
    return;
  }
  SpecializationIdentity identity(script);
  icScript->initSpecializationTrace(NextSpecializationId());
  SpecializationEvent("script", [&](JSONPrinter& json) {
    json.property("ic_script", icScript->specializationTraceId());
    json.formatProperty("script", "%p", static_cast<void*>(script));
    json.formatProperty("runtime", "%p",
                        static_cast<void*>(script->runtimeFromMainThread()));
    json.formatProperty("key", "%016" PRIx64, identity.key);
    json.formatProperty("source_hash", "%016" PRIx64, identity.sourceHash);
    json.formatProperty("bytecode_hash", "%016" PRIx64, identity.bytecodeHash);
    json.property("source_encoding", identity.encoding);
    json.property("identity_kind", identity.kind);
    json.property("immutable_flags", script->immutableFlags().toRaw());
    const char* filename = script->filename() ? script->filename() : "";
    StringProperty(json, "filename",
                   reinterpret_cast<const unsigned char*>(filename),
                   strlen(filename), false);
    json.property("source_start", script->sourceStart());
    json.property("source_end", script->sourceEnd());
    json.boolProperty("self_hosted", script->selfHosted());
    json.boolProperty("system_realm", script->realm()->isSystem());
    json.boolProperty("inlined", icScript->isInlined());
    json.property("num_ic_entries", icScript->numICEntries());
  });
}

static void SpecializationShape(JSONPrinter& json, Shape* shape) {
  json.beginObjectProperty("layout");
  json.property("class", shape->getObjectClass()->name);
  json.boolProperty("native", shape->isNative());
  json.boolProperty("dictionary", shape->isDictionary());
  json.boolProperty("null_proto", shape->proto() == TaggedProto(nullptr));
  json.boolProperty("prototype_unresolved",
                    shape->proto() != TaggedProto(nullptr));
  if (shape->isNative()) {
    NativeShape* native = &shape->asNative();
    json.property("fixed_slots", native->numFixedSlots());
    json.beginListProperty("properties");
    for (ShapePropertyIter<NoGC> iter(native); !iter.done(); iter++) {
      json.beginObject();
      auto key = iter->key();
      if (key.isInt()) {
        json.property("index", key.toInt());
      } else if (key.isAtom()) {
        JS::AutoCheckCannotGC nogc;
        JSAtom* atom = key.toAtom();
        if (atom->hasLatin1Chars()) {
          StringProperty(json, "name", atom->latin1Chars(nogc), atom->length());
        } else {
          StringProperty(json, "name", atom->twoByteChars(nogc),
                         atom->length());
        }
      } else {
        json.boolProperty("unresolved_symbol", true);
      }
      json.property("flags", uint32_t(iter->flags().toRaw()));
      if (iter->hasSlot()) {
        json.property("slot", iter->slot());
      }
      json.endObject();
    }
    json.endList();
  }
  json.endObject();
}

static void SpecializationStub(JSONPrinter& json, ICCacheIRStub* stub) {
  const CacheIRStubInfo* info = stub->stubInfo();
  json.beginObject();
  json.property("kind", uint32_t(info->kind()));
  json.property("entered", stub->enteredCount());
  json.property("type_data", uint32_t(stub->typeData().type()));
  HexProperty(json, "ir", info->code(), info->codeLength());
  json.beginListProperty("ops");
  CacheIRReader reader(info);
  while (reader.more()) {
    CacheOp op = reader.readOp();
    json.value("%s", CacheIRCodeName(op));
    reader.skip(CacheIROpInfos[size_t(op)].argLength);
  }
  json.endList();
  json.beginListProperty("fields");
  uint32_t offset = 0;
  for (uint32_t i = 0; info->fieldType(i) != StubField::Type::Limit; i++) {
    auto type = info->fieldType(i);
    bool scalar = type == StubField::Type::RawInt32 ||
                  type == StubField::Type::RawInt64 ||
                  type == StubField::Type::Double;
    if (type == StubField::Type::Value || type == StubField::Type::WeakValue) {
      uint64_t bits;
      memcpy(&bits, stub->stubDataStart() + offset, sizeof(bits));
      scalar = !Value::fromRawBits(bits).isGCThing();
    }
    json.beginObject();
    json.property("type", StubFieldTypeName(type));
    json.boolProperty("scalar", scalar);
    if (scalar) {
      HexProperty(json, "bits", stub->stubDataStart() + offset,
                  StubField::sizeInBytes(type));
    }
    if (type == StubField::Type::Shape || type == StubField::Type::WeakShape) {
      auto* shape =
          reinterpret_cast<Shape*>(info->getStubRawWord(stub, offset));
      if (shape) {
        SpecializationShape(json, shape);
      }
    }
    if (type == StubField::Type::String) {
      auto* str =
          reinterpret_cast<JSString*>(info->getStubRawWord(stub, offset));
      if (str && str->isLinear()) {
        JS::AutoCheckCannotGC nogc;
        auto& linear = str->asLinear();
        if (linear.hasLatin1Chars()) {
          StringProperty(json, "string", linear.latin1Chars(nogc),
                         linear.length());
        } else {
          StringProperty(json, "string", linear.twoByteChars(nogc),
                         linear.length());
        }
      }
    }
    if (type == StubField::Type::JSObject ||
        type == StubField::Type::WeakObject) {
      JSObject* obj =
          reinterpret_cast<JSObject*>(info->getStubRawWord(stub, offset));
      if (obj && obj->is<JSFunction>()) {
        JSFunction& fun = obj->as<JSFunction>();
        if (fun.hasBytecode()) {
          SpecializationIdentity target(fun.baseScript()->asJSScript());
          json.formatProperty("function_key", "%016" PRIx64, target.key);
          json.property("function_identity_kind", target.kind);
        }
      }
    }
    if (type == StubField::Type::WeakBaseScript) {
      auto* script =
          reinterpret_cast<BaseScript*>(info->getStubRawWord(stub, offset));
      if (script && script->hasBytecode()) {
        SpecializationIdentity target(script->asJSScript());
        json.formatProperty("function_key", "%016" PRIx64, target.key);
        json.property("function_identity_kind", target.kind);
      }
    }
    json.endObject();
    offset += StubField::sizeInBytes(type);
  }
  json.endList();
  json.endObject();
}

void TraceSpecializationSelected(ICScript* icScript, ICCacheIRStub* stub,
                                 uint32_t offset, uint64_t compilation,
                                 uint64_t context) {
  SpecializationEvent("selected", [&](JSONPrinter& json) {
    json.property("ic_script", icScript->specializationTraceId());
    json.property("compilation", compilation);
    json.property("offset", offset);
    json.property("context", context);
    json.beginListProperty("stubs");
    SpecializationStub(json, stub);
    json.endList();
  });
}

uint64_t TraceSpecializationProfile(JSScript* script, ICScript* icScript,
                                    uint64_t compilation, uint64_t parent,
                                    uint32_t callOffset, int32_t osrOffset) {
  if (!compilation) {
    return 0;
  }
  uint64_t context = NextSpecializationId();
  SpecializationEvent("profile", [&](JSONPrinter& json) {
    json.property("ic_script", icScript->specializationTraceId());
    json.property("compilation", compilation);
    json.property("context", context);
    json.property("parent", parent);
    json.property("call_offset", callOffset);
    json.property("osr_offset", osrOffset);
    json.boolProperty("failed_bounds_check", script->failedBoundsCheck());
    json.boolProperty("failed_lexical_check", script->failedLexicalCheck());
    json.property("num_ic_entries", icScript->numICEntries());
    json.beginListProperty("sites");
    for (size_t i = 0; i < icScript->numICEntries(); i++) {
      ICFallbackStub* fallback = icScript->fallbackStub(i);
      json.beginObject();
      json.property("offset", fallback->pcOffset());
      json.property("mode", uint32_t(fallback->state().mode()));
      json.boolProperty("has_failures", fallback->state().hasFailures());
      json.property("trial_inlining", uint32_t(fallback->trialInliningState()));
      json.boolProperty("may_have_folded", fallback->mayHaveFoldedStub());
      json.property("fallback_entered", fallback->enteredCount());
      json.beginListProperty("stubs");
      for (ICStub* stub = icScript->icEntry(i).firstStub(); !stub->isFallback();
           stub = stub->maybeNext()) {
        SpecializationStub(json, stub->toCacheIRStub());
      }
      json.endList();
      json.endObject();
    }
    json.endList();
  });
  return context;
}

void TraceSpecializationProfileEnd(uint64_t compilation, uint64_t context,
                                   WarpScriptSnapshot* snapshot) {
  SpecializationEvent("profile_end", [&](JSONPrinter& json) {
    json.property("compilation", compilation);
    json.property("context", context);
    json.beginListProperty("snapshots");
    for (const auto* op : snapshot->opSnapshots()) {
      json.beginObject();
      json.property("offset", op->offset());
      switch (op->kind()) {
#define TRACE_KIND(name)           \
  case WarpOpSnapshot::Kind::name: \
    json.property("kind", #name);  \
    break;
        WARP_OP_SNAPSHOT_LIST(TRACE_KIND)
#undef TRACE_KIND
      }
      json.endObject();
    }
    json.endList();
  });
}

void TraceSpecializationDecision(uint64_t compilation, uint64_t context,
                                 uint32_t offset, const char* decision) {
  SpecializationEvent("decision", [&](JSONPrinter& json) {
    json.property("compilation", compilation);
    json.property("context", context);
    json.property("offset", offset);
    json.property("decision", decision);
  });
}

void TraceSpecializationTypes(uint64_t compilation, uint64_t context,
                              uint32_t offset, const TypeDataList& types) {
  SpecializationEvent("polymorphic_types", [&](JSONPrinter& json) {
    json.property("compilation", compilation);
    json.property("context", context);
    json.property("offset", offset);
    json.beginListProperty("types");
    for (auto type : types) {
      json.value(uint32_t(type.type()));
    }
    json.endList();
  });
}

uint64_t BeginSpecializationCompilation(JSScript* script) {
  if (!JS_SHOULD_LOG(specializationCache, Debug)) {
    return 0;
  }
  InitSpecializationTrace(script, script->jitScript()->icScript());
  uint64_t id = NextSpecializationId();
  SpecializationEvent("ion_request", [&](JSONPrinter& json) {
    json.property("ic_script",
                  script->jitScript()->icScript()->specializationTraceId());
    json.property("compilation", id);
    json.property("warmup", script->getWarmUpCount());
    json.property("last_stub_warmup", script->warmUpCountAtLastICStub());
  });
  return id;
}

void TraceSpecializationCompilation(MIRGenerator* mir, const char* phase,
                                    double durationUs, bool success) {
  if (!mir->specializationTraceId()) {
    return;
  }
  SpecializationEvent("ion_compile", [&](JSONPrinter& json) {
    json.property("compilation", mir->specializationTraceId());
    json.property("phase", phase);
    json.property("duration_us", int64_t(durationUs));
    json.boolProperty("success", success);
  });
}

void TraceSpecializationLink(JSScript* script, MIRGenerator* mir,
                             double durationUs, bool success) {
  SpecializationEvent("ion_link", [&](JSONPrinter& json) {
    json.property("ic_script",
                  script->jitScript()->icScript()->specializationTraceId());
    json.property("compilation", mir->specializationTraceId());
    json.property("duration_us", int64_t(durationUs));
    json.boolProperty("success", success && script->hasIonScript());
    if (success && script->hasIonScript()) {
      script->ionScript()->setSpecializationTraceId(
          mir->specializationTraceId());
      json.property(
          "code_bytes",
          uint64_t(script->ionScript()->method()->instructionsSize()));
      json.property("metadata_bytes",
                    uint64_t(script->ionScript()->allocBytes()));
    }
  });
}

void TraceSpecializationDependencies(MIRGenerator* mir) {
  SpecializationEvent("dependencies", [&](JSONPrinter& json) {
    json.property("compilation", mir->specializationTraceId());
    json.beginListProperty("types");
    for (auto iter = mir->tracker.dependencies.iter(); !iter.done();
         iter.next()) {
      CompilationDependency* dependency = iter.get();
      json.value(int(dependency->type));
    }
    json.endList();
  });
}

void TraceSpecializationBailout(JSScript* script, const char* kind) {
  SpecializationEvent("bailout", [&](JSONPrinter& json) {
    json.property("ic_script",
                  script->jitScript()->icScript()->specializationTraceId());
    json.property("compilation",
                  script->hasIonScript()
                      ? script->ionScript()->specializationTraceId()
                      : 0);
    json.property("kind", kind);
  });
}

void TraceSpecializationInvalidation(JSScript* script) {
  SpecializationEvent("invalidate", [&](JSONPrinter& json) {
    json.property("ic_script",
                  script->jitScript()->icScript()->specializationTraceId());
    json.property("compilation", script->ionScript()->specializationTraceId());
  });
}

}  // namespace js::jit
