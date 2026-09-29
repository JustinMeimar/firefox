/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTRecorder.h"

#  include "mozilla/ScopeExit.h"
#  include "mozilla/SHA1.h"

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

// Filename helpers

static void HexEncode(const uint8_t* bytes, size_t len, char* out) {
  static const char Hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = Hex[bytes[i] >> 4];
    out[2 * i + 1] = Hex[bytes[i] & 0x0f];
  }
  out[2 * len] = '\0';
}

static void HashKey(mozilla::Span<const uint8_t> key,
                    mozilla::SHA1Sum::Hash& hash) {
  mozilla::SHA1Sum sha;
  sha.update(key.data(), key.size());
  sha.finish(hash);
}

// AOTArtifactRecorder

bool AOTArtifactRecorder::init(JSContext* cx, const char* dir) {
  directory_.assign(dir);
  if (mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) {
    JS_ReportErrorASCII(cx, "AOT record dir mkdir failed: %s: %s",
                        directory_.c_str(), strerror(errno));
    return false;
  }
  return true;
}

void AOTArtifactRecorder::writeBlobFile(
    const std::string& path, const AOTBlobWriter& blob,
    mozilla::Span<const AOTLinkSite> sites) {
  if (failed_) {
    return;
  }
  // IC attachment forbids exceptions, and debug OSR cannot report an error
  // while rewriting the stack. The shell checks failed_ after execution.
  auto reportError = [&](const char* message, int error = 0) {
    failed_ = true;
    if (error) {
      fprintf(stderr, "%s: %s: %s\n", message, path.c_str(), strerror(error));
    } else {
      fprintf(stderr, "%s: %s\n", message, path.c_str());
    }
  };
  if (blob.key().size() > UINT32_MAX || blob.fields().size() > UINT32_MAX ||
      blob.arrays().size() > UINT32_MAX || blob.code().size() > UINT32_MAX ||
      sites.size() > UINT32_MAX / sizeof(AOTLinkSite)) {
    return reportError("AOT artifact exceeds format limits");
  }
  std::string temporaryPath = path + ".tmp.XXXXXX";
  int fd = mkstemp(temporaryPath.data());
  if (fd < 0) {
    return reportError("AOT record open failed", errno);
  }
  auto cleanup = mozilla::MakeScopeExit([&] {
    if (fd >= 0) {
      close(fd);
    }
    unlink(temporaryPath.c_str());
  });

  AOTBlobFileHeader hdr = {};
  hdr.magic = BlobFileMagic;
  hdr.version = BlobFileVersion;
  hdr.kind = uint32_t(blob.kind());
  hdr.probeHash = blob.probeHash();
  memcpy(hdr.identityHash, blob.identityHash(), sizeof(hdr.identityHash));
  hdr.fieldsSize = uint32_t(blob.fields().size());
  hdr.arraysSize = uint32_t(blob.arrays().size());
  hdr.codeSize = uint32_t(blob.code().size());
  hdr.linkSitesSize = uint32_t(sites.size() * sizeof(AOTLinkSite));
  hdr.slotTableHash = AOTImageLinkHash();
  hdr.keySize = uint32_t(blob.key().size());
  memcpy(hdr.buildIdentity, CurrentAOTBuildIdentity().data(),
         sizeof(hdr.buildIdentity));

  auto writeBytes = [&](const void* p, size_t n) -> bool {
    const uint8_t* cur = static_cast<const uint8_t*>(p);
    while (n) {
      ssize_t rc = write(fd, cur, n);
      if (rc <= 0) {
        if (rc < 0 && errno == EINTR) continue;
        reportError("AOT record write failed", rc == 0 ? EIO : errno);
        return false;
      }
      cur += rc;
      n -= size_t(rc);
    }
    return true;
  };

  if (!writeBytes(&hdr, sizeof(hdr)) ||
      !writeBytes(blob.key().data(), blob.key().size()) ||
      !writeBytes(blob.fields().data(), blob.fields().size()) ||
      !writeBytes(blob.arrays().data(), blob.arrays().size()) ||
      !writeBytes(blob.code().data(), blob.code().size()) ||
      !writeBytes(sites.data(), hdr.linkSitesSize)) {
    return;
  }
  int rc = close(fd);
  fd = -1;
  if (rc != 0) {
    return reportError("AOT record close failed", errno);
  }

  // Publish complete artifacts without replacing a concurrent writer's file.
  if (link(temporaryPath.c_str(), path.c_str()) == 0) {
    return;
  }
  if (errno != EEXIST) {
    return reportError("AOT record link failed", errno);
  }

  FILE* existing = fopen(path.c_str(), "rb");
  if (!existing) {
    return reportError("Cannot read existing AOT artifact", errno);
  }
  auto closeExisting = mozilla::MakeScopeExit([&] { fclose(existing); });
  auto matches = [&](const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint8_t buffer[4096];
    while (length) {
      size_t count = std::min(length, sizeof(buffer));
      if (fread(buffer, 1, count, existing) != count ||
          memcmp(buffer, bytes, count) != 0) {
        return false;
      }
      bytes += count;
      length -= count;
    }
    return true;
  };
  if (!matches(&hdr, sizeof(hdr)) ||
      !matches(blob.key().data(), blob.key().size()) ||
      !matches(blob.fields().data(), blob.fields().size()) ||
      !matches(blob.arrays().data(), blob.arrays().size()) ||
      !matches(blob.code().data(), blob.code().size()) ||
      !matches(sites.data(), hdr.linkSitesSize) || fgetc(existing) != EOF ||
      ferror(existing)) {
    return reportError("Conflicting or incomplete AOT artifact");
  }
}

template <typename Metadata>
bool AOTArtifactRecorder::record(JSContext* cx, JitCode* code, AOTBlobKind kind,
                                 mozilla::Span<const uint8_t> key,
                                 uint32_t probeHash, const Metadata& md,
                                 bool (*encode)(AOTBlobWriter&,
                                                const Metadata&),
                                 mozilla::Span<const AOTLinkSite> sites) {
  mozilla::SHA1Sum::Hash identity;
  HashKey(key, identity);
  AOTBlobWriter blob(kind, probeHash, identity);
  if (!blob.writeKey(key) || !encode(blob, md) ||
      !blob.writeCode(code->raw(), code->instructionsSize())) {
    if (kind != AOTBlobKind::InlineCacheStub) {
      ReportOutOfMemory(cx);
    }
    return false;
  }
  char idHex[2 * sizeof(identity) + 1];
  HexEncode(identity, sizeof(identity), idHex);
  std::string path =
      directory_ + "/" + AOTArtifactPrefix(kind) + "-" + idHex + ".aotb";
  writeBlobFile(path, blob, sites);
  if (failed_) {
    JS_ReportErrorASCII(cx, "AOT artifact publication failed");
    return false;
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
