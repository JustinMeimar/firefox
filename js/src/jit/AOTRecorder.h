/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTRecorder_h
#define jit_AOTRecorder_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Span.h"

#  include <cstdint>
#  include <string>

#  include "jstypes.h"

#  include "jit/AOT.h"
#  include "jit/AOTImage.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class JitCode;
class BaselineInterpreter;
struct BaselineInterpreterMetadata;
struct BaselineScriptMetadata;
struct AOTICStubMetadata;

// [SMDOC] AOT Artifact Recorder
// =============================
//
// Records captured AOT artifacts as individual files. Each file contains
// generated code and the metadata required to rebuild the corresponding
// runtime object. Identity based names and exclusive creation deduplicate
// artifacts across concurrent processes.
class AOTArtifactRecorder {
 public:
  AOTArtifactRecorder() = default;
  AOTArtifactRecorder(const AOTArtifactRecorder&) = delete;
  AOTArtifactRecorder& operator=(const AOTArtifactRecorder&) = delete;

  // Creates the recording directory if it does not already exist.
  [[nodiscard]] bool init(JSContext* cx, const char* dir);

  const std::string& directory() const { return directory_; }
  bool failed() const { return failed_; }

  // Each record entry point takes the link sites the capturing assembler
  // collected. Their offsets are relative to the artifact's own code.
  // Publication errors set failed(); allocation errors return false.

  // Interpreter variants are named from their compilation context.
  [[nodiscard]] bool recordInterpreter(JSContext* cx, JitCode* code,
                                       const BaselineInterpreterMetadata& md,
                                       mozilla::Span<const AOTLinkSite> sites);

  // Baseline function artifacts are named from a hash of the script state that
  // affects compilation.
  [[nodiscard]] bool recordBaselineFunction(
      JSContext* cx, JitCode* code, mozilla::Span<const uint8_t> key,
      uint32_t probeHash, const BaselineScriptMetadata& md,
      mozilla::Span<const AOTLinkSite> sites);

  // Inline cache identities cover the cache kind, encoded operations, and field
  // types. Artifact file names include the full hash.
  [[nodiscard]] bool recordICStub(JSContext* cx, JitCode* code,
                                  const AOTICStubMetadata& md,
                                  mozilla::Span<const AOTLinkSite> sites);

  // Records each self hosted function by delazifying it and triggering baseline
  // compilation. Returns the number of artifacts recorded. An empty result is
  // valid.
  [[nodiscard]] bool recordSelfHostedBaselineCorpus(JSContext* cx,
                                                    uint32_t* compiledOut,
                                                    uint32_t* skippedOut);

 private:
  template <typename Metadata>
  [[nodiscard]] bool record(JSContext* cx, JitCode* code, AOTBlobKind kind,
                            mozilla::Span<const uint8_t> key,
                            uint32_t probeHash, const Metadata& md,
                            bool (*encode)(AOTBlobWriter&, const Metadata&),
                            mozilla::Span<const AOTLinkSite> sites);

  std::string directory_;
  bool failed_ = false;
  bool selfHostedComplete_ = false;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTRecorder_h
