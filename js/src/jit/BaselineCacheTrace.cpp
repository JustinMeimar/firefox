/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCacheTrace.h"

#include <cstring>
#include <limits>

#include "jit/BaselineJIT.h"
#include "jit/JitCode.h"
#include "js/Utility.h"
#include "util/GetPidProvider.h"
#include "vm/JSScript.h"
#include "vm/Logging.h"
#include "vm/Realm.h"
#include "vm/Time.h"

namespace js::jit {

static uint64_t HashBaselineTraceBytes(const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < length; i++) {
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  }
  return hash;
}

static JS::UniqueChars HexBaselineTraceFilename(const char* filename) {
  if (!filename) {
    return nullptr;
  }
  size_t length = strlen(filename);
  if (length > (std::numeric_limits<size_t>::max() - 1) / 2) {
    return nullptr;
  }
  JS::UniqueChars hex(static_cast<char*>(js_malloc(length * 2 + 1)));
  if (!hex) {
    return nullptr;
  }
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; i++) {
    auto byte = static_cast<uint8_t>(filename[i]);
    hex[i * 2] = digits[byte >> 4];
    hex[i * 2 + 1] = digits[byte & 15];
  }
  hex[length * 2] = '\0';
  return hex;
}

void TraceBaselineCompile(JSScript* script, const char* phase,
                          int64_t startTime, bool success) {
  if (!JS_SHOULD_LOG(baselineCache, Debug)) {
    return;
  }
  int64_t endTime = PRMJ_Now();
  JS_LOG(baselineCache, Debug, "BCACHE\t1\tcompile\t%lld\t%d\t%p\t%s\t%lld\t%d",
         static_cast<long long>(endTime), getpid(), static_cast<void*>(script),
         phase, static_cast<long long>(endTime - startTime), int(success));
}

void TraceBaselineInstall(JSScript* script, BaselineScript* baseline,
                          int64_t startTime, int64_t installTime) {
  if (!JS_SHOULD_LOG(baselineCache, Debug)) {
    return;
  }

  ScriptSource* source = script->scriptSource();
  uint64_t sourceHash = 0;
  int sourceEncoding = 0;
  if (source->hasSourceText() && script->sourceEnd() <= source->length()) {
    size_t length = script->sourceLength();
    if (length == 0) {
      sourceHash = HashBaselineTraceBytes(nullptr, 0);
      sourceEncoding = source->hasSourceType<char16_t>() ? 16 : 8;
    } else {
      auto chars =
          source->substringChars(script->sourceStart(), script->sourceEnd());
      if (chars.is<JS::UniqueChars>()) {
        const auto& utf8 = chars.as<JS::UniqueChars>();
        if (utf8) {
          sourceHash = HashBaselineTraceBytes(utf8.get(), length);
          sourceEncoding = 8;
        }
      } else {
        const auto& utf16 = chars.as<JS::UniqueTwoByteChars>();
        if (utf16) {
          sourceHash =
              HashBaselineTraceBytes(utf16.get(), length * sizeof(char16_t));
          sourceEncoding = 16;
        }
      }
    }
  }

  uint64_t bytecodeHash =
      HashBaselineTraceBytes(script->code(), script->length());
  JS::UniqueChars filename = HexBaselineTraceFilename(script->filename());
  JS_LOG(baselineCache, Debug,
         "BCACHE\t1\tinstall\t%lld\t%d\t%p\t%p\t%p\t%p\t%u\t%d\t%d\t%d\t%u\t%"
         "u\t%u\t%u\t%u\t%u\t%llu\t%d\t%llu\t%zu\t%zu\t%zu\t%lld\t%s",
         static_cast<long long>(installTime), getpid(),
         static_cast<void*>(script), static_cast<void*>(baseline),
         static_cast<void*>(baseline->method()),
         static_cast<void*>(script->runtimeFromMainThread()), source->id(),
         int(script->realm()->isSystem()), int(script->selfHosted()),
         int(baseline->hasDebugInstrumentation()),
         script->immutableFlags().toRaw(), script->filenameHash(),
         script->sourceStart(), script->sourceEnd(), script->lineno(),
         script->column().oneOriginValue(),
         static_cast<unsigned long long>(sourceHash), sourceEncoding,
         static_cast<unsigned long long>(bytecodeHash),
         baseline->method()->instructionsSize(),
         baseline->method()->allocatedSize(), baseline->allocBytes(),
         static_cast<long long>(installTime - startTime),
         filename ? filename.get() : "");
}

void TraceBaselineDiscard(JSScript* script, BaselineScript* baseline) {
  if (!JS_SHOULD_LOG(baselineCache, Debug)) {
    return;
  }
  JS_LOG(baselineCache, Debug, "BCACHE\t1\tdiscard\t%lld\t%d\t%p\t%p",
         static_cast<long long>(PRMJ_Now()), getpid(),
         static_cast<void*>(script), static_cast<void*>(baseline));
}

}  // namespace js::jit
