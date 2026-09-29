/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTRecorder.h"

#  include "mozilla/Result.h"
#  include "mozilla/ScopeExit.h"
#  include "mozilla/SHA1.h"
#  include "mozilla/UniquePtrExtensions.h"

#  include <algorithm>
#  include <cerrno>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include "frontend/CompilationStencil.h"
#  include "gc/Zone.h"
#  include "jit/AOTCompilationKey.h"
#  include "jit/AOTImageGenerated.h"
#  include "jit/BaselineJIT.h"
#  include "jit/CacheIR.h"
#  include "jit/JitCode.h"
#  include "jit/JitOptions.h"
#  include "jit/JitRuntime.h"
#  include "jit/JitScript.h"
#  include "jit/JitSpewer.h"
#  include "jit/JitZone.h"
#  include "js/AOTRecording.h"
#  include "js/ErrorReport.h"
#  include "js/Printer.h"
#  include "js/Printf.h"
#  include "vm/GeckoProfiler.h"
#  include "vm/JSAtomUtils.h"
#  include "vm/JSContext.h"
#  include "vm/JSFunction.h"
#  include "vm/JSScript.h"
#  include "vm/Runtime.h"

#  include "jit/AOTABIFns-inl.h"
#  include "jit/JitScript-inl.h"
#  include "vm/JSObject-inl.h"

namespace js::jit {

namespace {

using ArtifactParts = mozilla::Span<const mozilla::Span<const uint8_t>>;
using UniqueFile = mozilla::UniquePtr<FILE, decltype(&fclose)>;

struct ArtifactIOError {
  const char* message;
  int error = 0;
};

bool FileMatches(FILE* file, ArtifactParts parts) {
  uint8_t buffer[4096];
  for (auto part : parts) {
    while (!part.empty()) {
      size_t count = std::min(part.size(), sizeof(buffer));
      if (fread(buffer, 1, count, file) != count ||
          memcmp(buffer, part.data(), count) != 0) {
        return false;
      }
      part = part.From(count);
    }
  }
  return fgetc(file) == EOF && !ferror(file);
}

mozilla::Result<mozilla::Ok, ArtifactIOError> PublishArtifact(
    const char* path, ArtifactParts parts) {
  auto temporaryPath = JS_smprintf("%s.tmp.XXXXXX", path);
  if (!temporaryPath) {
    return mozilla::Err(
        ArtifactIOError{"AOT temporary path allocation failed", ENOMEM});
  }
  mozilla::UniqueFileHandle fd(mkstemp(temporaryPath.get()));
  if (!fd) {
    return mozilla::Err(ArtifactIOError{"AOT record open failed", errno});
  }
  auto cleanup = mozilla::MakeScopeExit([&] { unlink(temporaryPath.get()); });
  UniqueFile file(fdopen(fd.get(), "wb"), fclose);
  if (!file) {
    return mozilla::Err(ArtifactIOError{"AOT record fdopen failed", errno});
  }
  (void)fd.release();

  for (auto part : parts) {
    if (!part.empty() &&
        fwrite(part.data(), 1, part.size(), file.get()) != part.size()) {
      return mozilla::Err(ArtifactIOError{"AOT record write failed", errno});
    }
  }
  if (fclose(file.release()) != 0) {
    return mozilla::Err(ArtifactIOError{"AOT record close failed", errno});
  }

  // Publish complete artifacts without replacing a concurrent writer's file.
  if (link(temporaryPath.get(), path) == 0) {
    return mozilla::Ok();
  }
  if (errno != EEXIST) {
    return mozilla::Err(ArtifactIOError{"AOT record link failed", errno});
  }
  UniqueFile existing(fopen(path, "rb"), fclose);
  if (!existing) {
    return mozilla::Err(
        ArtifactIOError{"Cannot read existing AOT artifact", errno});
  }
  if (!FileMatches(existing.get(), parts)) {
    return mozilla::Err(
        ArtifactIOError{"Conflicting or incomplete AOT artifact"});
  }
  return mozilla::Ok();
}

}  // namespace

bool AOTArtifactRecorder::init(JSContext* cx, const char* dir) {
  directory_.assign(dir);
  if (mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) {
    JS_ReportErrorASCII(cx, "AOT record dir mkdir failed: %s: %s",
                        directory_.c_str(), strerror(errno));
    return false;
  }
  return true;
}

template <typename Metadata, typename Fields, size_t N>
bool AOTArtifactRecorder::record(
    JSContext* cx, JitCode* code, AOTBlobKind kind,
    mozilla::Span<const uint8_t> key, uint32_t probeHash, const Metadata& md,
    AOTEncodedMetadata<Fields, N> (*encode)(const Metadata&),
    mozilla::Span<const AOTLinkSite> sites) {
  if (failed_) {
    return true;
  }
  auto blob = encode(md);
  size_t arraysSize = 0;
  for (auto array : blob.arrays) {
    arraysSize += array.size();
  }

  mozilla::SHA1Sum::Hash identity;
  mozilla::SHA1Sum sha;
  sha.update(key.data(), key.size());
  sha.finish(identity);
  char idHex[2 * sizeof(identity) + 1];
  FixedBufferPrinter printer(idHex, sizeof(idHex));
  for (uint8_t byte : identity) {
    printer.printf("%02x", unsigned(byte));
  }
  auto path = JS_smprintf("%s/%s-%s.aotb", directory_.c_str(),
                          AOTArtifactPrefix(kind), idHex);
  if (!path) {
    if (kind != AOTBlobKind::InlineCacheStub) {
      ReportOutOfMemory(cx);
    }
    return false;
  }

  // IC attachment forbids exceptions, and debug OSR cannot report an error
  // while rewriting the stack. The shell checks failed_ after execution.
  auto reportPublicationError = [&](ArtifactIOError error) {
    failed_ = true;
    fprintf(stderr, "%s: %s%s%s\n", error.message, path.get(),
            error.error ? ": " : "", error.error ? strerror(error.error) : "");
  };
  if (key.size() > UINT32_MAX || arraysSize > UINT32_MAX ||
      sites.size() > UINT32_MAX / sizeof(AOTLinkSite)) {
    reportPublicationError({"AOT artifact exceeds format limits"});
    return true;
  }

  AOTBlobFileHeader hdr = {};
  hdr.magic = BlobFileMagic;
  hdr.version = BlobFileVersion;
  hdr.kind = uint32_t(kind);
  hdr.probeHash = probeHash;
  memcpy(hdr.identityHash, identity, sizeof(hdr.identityHash));
  hdr.fieldsSize = sizeof(Fields);
  hdr.arraysSize = uint32_t(arraysSize);
  hdr.codeSize = uint32_t(code->instructionsSize());
  hdr.linkSitesSize = uint32_t(sites.size() * sizeof(AOTLinkSite));
  hdr.slotTableHash = AOTImageLinkHash();
  hdr.keySize = uint32_t(key.size());
  memcpy(hdr.buildIdentity, CurrentAOTBuildIdentity().data(),
         sizeof(hdr.buildIdentity));

  mozilla::Span<const uint8_t> parts[N + 5] = {
      mozilla::AsBytes(mozilla::Span(&hdr, 1)), key,
      mozilla::AsBytes(mozilla::Span(&blob.fields, 1))};
  for (size_t i = 0; i < N; i++) {
    parts[3 + i] = blob.arrays[i];
  }
  parts[N + 3] = {code->raw(), code->instructionsSize()};
  parts[N + 4] = mozilla::AsBytes(sites);
  auto result = PublishArtifact(path.get(), parts);
  if (result.isErr()) {
    reportPublicationError(result.unwrapErr());
  }
  return true;
}

bool AOTArtifactRecorder::recordInterpreter(
    JSContext* cx, JitCode* code, const BaselineInterpreterMetadata& md,
    mozilla::Span<const AOTLinkSite> sites) {
  AOTCompilationKey key;
  WriteAOTContext(key, AOTBlobKind::BaselineInterpreter, JitOptions,
                  cx->runtime()->geckoProfiler().enabled());
  if (!key.complete()) {
    ReportOutOfMemory(cx);
    return false;
  }
  return record(cx, code, AOTBlobKind::BaselineInterpreter, key.data(), 0, md,
                EncodeBlob_BaselineInterpreter, sites);
}

bool AOTArtifactRecorder::recordBaselineFunction(
    JSContext* cx, JitCode* code, mozilla::Span<const uint8_t> key,
    uint32_t probeHash, const BaselineScriptMetadata& md,
    mozilla::Span<const AOTLinkSite> sites) {
  return record(cx, code, AOTBlobKind::BaselineFunction, key, probeHash, md,
                EncodeBlob_BaselineFunction, sites);
}

bool AOTArtifactRecorder::recordSelfHostedBaselineCorpus(JSContext* cx,
                                                         uint32_t* compiledOut,
                                                         uint32_t* skippedOut) {
  *compiledOut = 0;
  *skippedOut = 0;

  if (selfHostedComplete_) {
    return true;
  }

  if (!cx->runtime()->hasSelfHostStencil()) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT self-hosted corpus: no self-host stencil loaded");
    return true;
  }

  JS::RootedVector<JSAtom*> names(cx);
  {
    auto& map = cx->runtime()->selfHostScriptMap.ref();
    if (!names.reserve(map.count())) {
      return false;
    }
    for (auto iter = map.iter(); !iter.done(); iter.next()) {
      names.infallibleAppend(iter.get().key());
    }
  }

  // Self hosted function instantiation must not invoke the allocation metadata
  // builder.
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

  for (JSAtom* rawAtom : names.get()) {
    Rooted<JSAtom*> atom(cx, rawAtom);
    Rooted<PropertyName*> name(cx, atom->asPropertyName());
    auto indexRange = cx->runtime()->getSelfHostedScriptIndexRange(name);
    if (!indexRange) {
      (*skippedOut)++;
      continue;
    }

    RootedFunction fun(
        cx, cx->runtime()->selfHostStencil().instantiateSelfHostedLazyFunction(
                cx, cx->runtime()->selfHostStencilInput().atomCache,
                indexRange->start, name));
    if (!fun) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }
    if (!cx->runtime()->delazifySelfHostedFunction(cx, name, fun)) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }

    // Trigger baseline compilation here so the recorder receives the generated
    // artifact through the normal compilation path.
    Rooted<JSScript*> script(cx, fun->nonLazyScript());
    if (!script || !CanBaselineInterpretScript(script)) {
      (*skippedOut)++;
      continue;
    }
    if (!cx->zone()->ensureJitZoneExists(cx)) {
      return false;
    }
    AutoKeepJitScripts keepJitScript(cx);
    if (!script->ensureHasJitScript(cx, keepJitScript)) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }

    BaselineOptions options({BaselineOption::ForceMainThreadCompilation});
    MethodStatus status = BaselineCompile(cx, script, options);
    if (status == Method_Error) {
      return false;
    }
    if (status != Method_Compiled) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }
    (*compiledOut)++;
  }

  selfHostedComplete_ = true;
  JitSpew(JitSpew_BaselineAOT, "AOT self-hosted corpus: recorded=%u skipped=%u",
          *compiledOut, *skippedOut);
  return true;
}

bool AOTArtifactRecorder::recordICStub(JSContext* cx, JitCode* code,
                                       const AOTICStubMetadata& md,
                                       mozilla::Span<const AOTLinkSite> sites) {
  AOTCompilationKey key;
  WriteAOTContext(key, AOTBlobKind::InlineCacheStub, JitOptions,
                  cx->runtime()->geckoProfiler().enabled());
  WriteAOTICInputs(key, md);
  if (!key.complete()) {
    return false;
  }
  return record(cx, code, AOTBlobKind::InlineCacheStub, key.data(), 0, md,
                EncodeBlob_InlineCacheStub, sites);
}

}  // namespace js::jit

JS_PUBLIC_API bool JS::MaybeRecordAOTSelfHostedBaselineCorpus(JSContext* cx) {
  js::jit::JitRuntime* jrt = cx->runtime()->jitRuntime();
  if (!jrt || !jrt->aotPolicy().recordSelfHosted()) {
    return true;
  }
  js::jit::AOTArtifactRecorder* rec = jrt->aotRecorder();
  if (!rec) {
    return true;
  }
  uint32_t compiled = 0, skipped = 0;
  if (!rec->recordSelfHostedBaselineCorpus(cx, &compiled, &skipped)) {
    return false;
  }
  return true;
}

#endif  // ENABLE_JS_AOT
