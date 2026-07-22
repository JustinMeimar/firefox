/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTRecorder_h
#define jit_AOTRecorder_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/HashTable.h"

#  include <cstdint>
#  include <string>

#  include "jstypes.h"

#  include "jit/AOTImage.h"
#  include "js/AllocPolicy.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class JitCode;
class BaselineInterpreter;
struct BaselineInterpreterMetadata;
struct BaselineScriptMetadata;
struct CacheIRStubKey;

// [SMDOC] AOT Artifact Recorder
// =============================
//
// Owned by JitRuntime; created lazily when JitOptions.aotRecordDir is
// set. Every AOT capture point (baseline interpreter, baseline JIT'd
// function, IC stub) hands the recorder a linked JitCode plus the
// metadata needed to reconstruct the artifact at load time. The
// recorder writes one .aotb file per artifact into the record
// directory, keyed by identity, using O_CREAT|O_EXCL so parallel test
// shells dedupe for free.
//
// Later (patch 13) PackAOTImage.py scans the directory, applies
// SelectAOTCorpus.py's budget, and packs the survivors into a single
// AOTImage.bin. That AOTImage.bin is the input to the incbin object
// linked into the shell (patch 11).
class AOTArtifactRecorder {
 public:
  AOTArtifactRecorder() = default;
  AOTArtifactRecorder(const AOTArtifactRecorder&) = delete;
  AOTArtifactRecorder& operator=(const AOTArtifactRecorder&) = delete;

  // Prepare `dir` for use. Creates the directory if missing.
  [[nodiscard]] bool init(JSContext* cx, const char* dir);

  const std::string& directory() const { return directory_; }

  // Baseline interpreter is a singleton: filename is always
  // <dir>/interp.aotb.
  [[nodiscard]] bool recordInterpreter(JSContext* cx, JitCode* code,
                                       const BaselineInterpreterMetadata& md);

  // Baseline function is keyed by identityHash (SHA-1 over the
  // compiler-relevant subset of JSScript state). Filename is
  // <dir>/blfun-<identity16>.aotb.
  [[nodiscard]] bool recordBaselineFunction(
      JSContext* cx, JitCode* code, const uint8_t identityHash[20],
      uint32_t probeHash, const BaselineScriptMetadata& md);

  // IC stub is keyed by CacheIRStubKey hash. Filename is
  // <dir>/ic-<identity16>.aotb.
  [[nodiscard]] bool recordICStub(JSContext* cx, JitCode* code,
                                  const CacheIRStubKey& key);

 private:
  using SeenSet =
      mozilla::HashSet<uint64_t, mozilla::DefaultHasher<uint64_t>,
                       SystemAllocPolicy>;

  [[nodiscard]] bool writeBlobFile(JSContext* cx, const std::string& path,
                                   const AOTBlobWriter& blob);

  // In-process dedupe. Cross-process dedupe is by O_CREAT|O_EXCL.
  bool wasSeen(const uint8_t identityHash[20]);

  std::string directory_;
  SeenSet seen_;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTRecorder_h
