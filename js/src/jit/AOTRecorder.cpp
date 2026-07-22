/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTRecorder.h"

#  include "mozilla/ScopeExit.h"

#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include <cerrno>
#  include <cstdio>
#  include <cstring>

#  include "jit/AOTImageGenerated.h"
#  include "jit/BaselineJIT.h"
#  include "jit/CacheIR.h"
#  include "jit/JitCode.h"
#  include "jit/JitSpewer.h"
#  include "jit/JitZone.h"
#  include "vm/JSContext.h"

namespace js::jit {

// ------------------------------------------------------------------
// Filename helpers
// ------------------------------------------------------------------

static void HexEncode(const uint8_t* bytes, size_t len, char* out) {
  static const char Hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = Hex[bytes[i] >> 4];
    out[2 * i + 1] = Hex[bytes[i] & 0x0f];
  }
  out[2 * len] = '\0';
}

static uint64_t Prefix64(const uint8_t* bytes) {
  uint64_t v;
  memcpy(&v, bytes, sizeof(v));
  return v;
}

// ------------------------------------------------------------------
// AOTArtifactRecorder
// ------------------------------------------------------------------

bool AOTArtifactRecorder::init(JSContext* cx, const char* dir) {
  directory_.assign(dir);
  if (mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) {
    JitSpew(JitSpew_BaselineAOT, "AOT record dir mkdir failed: %s: %s",
            directory_.c_str(), strerror(errno));
    return false;
  }
  return true;
}

bool AOTArtifactRecorder::wasSeen(const uint8_t identityHash[20]) {
  uint64_t key = Prefix64(identityHash);
  auto p = seen_.lookupForAdd(key);
  if (p) {
    return true;
  }
  (void)seen_.add(p, key);
  return false;
}

bool AOTArtifactRecorder::writeBlobFile(JSContext* cx, const std::string& path,
                                        const AOTBlobWriter& blob) {
  // O_EXCL: filename encodes identity, so first writer wins. Concurrent
  // record shells never race.
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      return true;
    }
    JitSpew(JitSpew_BaselineAOT, "AOT record open failed: %s: %s",
            path.c_str(), strerror(errno));
    return false;
  }
  auto closeGuard = mozilla::MakeScopeExit([&] { close(fd); });

  AOTBlobFileHeader hdr = {};
  hdr.magic = BlobFileMagic;
  hdr.version = BlobFileVersion;
  hdr.kind = uint32_t(blob.kind());
  hdr.probeHash = blob.probeHash();
  memcpy(hdr.identityHash, blob.identityHash(), sizeof(hdr.identityHash));
  hdr.fieldsSize = uint32_t(blob.fields().size());
  hdr.arraysSize = uint32_t(blob.arrays().size());
  hdr.codeSize = uint32_t(blob.code().size());

  auto writeBytes = [&](const void* p, size_t n) -> bool {
    const uint8_t* cur = static_cast<const uint8_t*>(p);
    while (n) {
      ssize_t rc = write(fd, cur, n);
      if (rc < 0) {
        if (errno == EINTR) continue;
        JitSpew(JitSpew_BaselineAOT, "AOT record write failed: %s: %s",
                path.c_str(), strerror(errno));
        return false;
      }
      cur += rc;
      n -= size_t(rc);
    }
    return true;
  };

  return writeBytes(&hdr, sizeof(hdr)) &&
         writeBytes(blob.fields().data(), blob.fields().size()) &&
         writeBytes(blob.arrays().data(), blob.arrays().size()) &&
         writeBytes(blob.code().data(), blob.code().size());
}

bool AOTArtifactRecorder::recordInterpreter(
    JSContext* cx, JitCode* code, const BaselineInterpreterMetadata& md) {
  AOTBlobWriter blob(AOTBlobKind::BaselineInterpreter, /* probeHash = */ 0,
                     /* identityHash = */ nullptr);
  if (!EncodeBlob_BaselineInterpreter(blob, md)) return false;
  if (!blob.writeCode(code->raw(), code->instructionsSize())) return false;

  std::string path = directory_ + "/interp.aotb";
  return writeBlobFile(cx, path, blob);
}

bool AOTArtifactRecorder::recordBaselineFunction(
    JSContext* cx, JitCode* code, const uint8_t identityHash[20],
    uint32_t probeHash, const BaselineScriptMetadata& md) {
  if (wasSeen(identityHash)) return true;

  AOTBlobWriter blob(AOTBlobKind::BaselineFunction, probeHash, identityHash);
  if (!EncodeBlob_BaselineFunction(blob, md)) return false;
  if (!blob.writeCode(code->raw(), code->instructionsSize())) return false;

  char idHex[41];
  HexEncode(identityHash, 8, idHex);
  std::string path = directory_ + "/blfun-" + idHex + ".aotb";
  return writeBlobFile(cx, path, blob);
}

bool AOTArtifactRecorder::recordICStub(JSContext* cx, JitCode* code,
                                       const CacheIRStubKey& key) {
  // Placeholder identity: SHA-1 over the stub's writer bytes is
  // computed by CacheIRWriter callers upstream. For now, use the raw
  // stub-info pointer as a stand-in so file names stay unique in a
  // single run. Patch 12 will replace this with the real SHA-1.
  uint8_t identity[20] = {};
  uintptr_t infoPtr = reinterpret_cast<uintptr_t>(key.stubInfo.get());
  memcpy(identity, &infoPtr, sizeof(infoPtr));

  if (wasSeen(identity)) return true;

  AOTBlobWriter blob(AOTBlobKind::InlineCacheStub, /* probeHash = */ 0,
                     identity);
  if (!blob.writeCode(code->raw(), code->instructionsSize())) return false;

  char idHex[41];
  HexEncode(identity, 8, idHex);
  std::string path = directory_ + "/ic-" + idHex + ".aotb";
  return writeBlobFile(cx, path, blob);
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT
