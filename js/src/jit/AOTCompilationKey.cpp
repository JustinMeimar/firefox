/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOTCompilationKey.h"

#include "mozilla/EndianUtils.h"

#include "vm/JSScript.h"
#include "vm/Scope.h"
#include "wasm/WasmCompile.h"

#include "vm/JSScript-inl.h"

namespace js::jit {

void AOTCompilationKey::append(mozilla::Span<const uint8_t> value) {
  if (!valid_) {
    return;
  }
  if (comparing_) {
    valid_ = value.size() <= expected_.size() - cursor_ &&
             (value.empty() || memcmp(expected_.data() + cursor_, value.data(),
                                      value.size()) == 0);
    if (valid_) {
      cursor_ += value.size();
    }
  } else {
    valid_ = data_.append(value.data(), value.size());
  }
}

void AOTCompilationKey::scalar(uint32_t value) {
  uint8_t bytes[sizeof(value)];
  mozilla::LittleEndian::writeUint32(bytes, value);
  append(bytes);
}

void AOTCompilationKey::bytes(mozilla::Span<const uint8_t> value) {
  if (value.size() > UINT32_MAX) {
    valid_ = false;
    return;
  }
  scalar(uint32_t(value.size()));
  append(value);
  const uint8_t padding[3] = {};
  append({padding, (4 - value.size() % 4) % 4});
}

void WriteAOTContext(AOTCompilationKey& key, AOTBlobKind kind,
                     const DefaultJitOptions& options, bool profiling) {
  key.scalar(uint32_t(kind));
  key.bytes(CurrentAOTBuildIdentity());
  key.scalar(wasm::ObservedCPUFeatures());
  key.scalar(options.spectreIndexMasking);
  key.scalar(options.spectreObjectMitigations);
  key.scalar(options.spectreStringMitigations);
  key.scalar(options.lessDebugCode);
  key.scalar(kind == AOTBlobKind::BaselineFunction && profiling);
  switch (kind) {
    case AOTBlobKind::BaselineInterpreter:
      key.scalar(options.disableInlining);
      key.scalar(options.baselineJit);
      key.scalar(options.baselineBatching);
      key.scalar(options.baselineQueueCapacity);
      return;
    case AOTBlobKind::BaselineFunction:
      key.scalar(options.disableInlining);
      key.scalar(options.ionMaxScriptSizeMainThread);
      key.scalar(options.ionMaxLocalsAndArgsMainThread);
      key.scalar(options.eagerIonCompilation());
      return;
    case AOTBlobKind::InlineCacheStub:
      key.scalar(options.enableICFramePointers);
      key.scalar(options.fullDebugChecks);
      return;
  }
  MOZ_CRASH("Invalid AOT artifact kind");
}

uint32_t AOTICContext() {
  return uint32_t(JitOptions.spectreIndexMasking) << 0 |
         uint32_t(JitOptions.spectreObjectMitigations) << 1 |
         uint32_t(JitOptions.spectreStringMitigations) << 2 |
         uint32_t(JitOptions.lessDebugCode) << 3 |
         uint32_t(JitOptions.enableICFramePointers) << 4 |
         uint32_t(JitOptions.fullDebugChecks) << 5;
}

void WriteAOTBaselineInputs(AOTCompilationKey& key, JSScript* script,
                            uint32_t baseWarmUpThreshold, bool ionCompileable,
                            bool debugInstrumentation) {
  key.scalar(baseWarmUpThreshold);
  key.scalar(ionCompileable);
  key.scalar(debugInstrumentation);
  key.scalar(script->immutableFlags().toRaw());
  key.scalar(script->function() ? script->function()->flags().toRaw() : 0);
  key.scalar(script->function() ? script->function()->nargs() : 0);
  key.scalar(script->nfixed());
  key.scalar(script->nslots());
  key.scalar(script->numICEntries());
  key.scalar(uint32_t(script->outermostScope()->kind()));
  key.scalar(script->hasNonSyntacticScope());
  key.scalar(!!script->function());
  key.scalar(script->needsArgsObj());
  key.scalar(script->argsObjAliasesFormals());
  key.scalar(uint32_t(script->gcthings().size()));
  for (const auto& thing : script->gcthings()) {
    key.scalar(uint32_t(thing.kind()));
  }
  key.bytes(script->immutableScriptData()->immutableData());
}

void WriteAOTICInputs(AOTCompilationKey& key, const AOTICStubMetadata& md) {
  key.scalar(md.cacheKind);
  key.scalar(md.stubDataOffset);
  key.bytes(mozilla::Span(md.cacheIRCode.begin(), md.cacheIRCode.length()));
  key.bytes(mozilla::Span(md.fieldTypes.begin(), md.fieldTypes.length()));
}

}  // namespace js::jit
