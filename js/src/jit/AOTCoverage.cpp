/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTCoverage.h"
#  include "js/AOTCoverage.h"

#  include "mozilla/JSONWriter.h"
#  include "mozilla/UniquePtr.h"

#  include <atomic>
#  include <cstdio>
#  include <cstdlib>
#  include <mutex>
#  include <string>
#  include <unistd.h>
#  include <vector>

#  include "jit/AOTImage.h"
#  include "jit/JitCode.h"
#  include "js/HashTable.h"

namespace js::jit {

namespace {

struct BaselineBlobCounts {
  bool selfHosted = false;
  uint64_t installs = 0;
};

using BaselineBlobMap =
    js::HashMap<uint32_t, BaselineBlobCounts, js::DefaultHasher<uint32_t>,
                js::SystemAllocPolicy>;
using ICBlobMap = js::HashMap<uint32_t, uint64_t, js::DefaultHasher<uint32_t>,
                              js::SystemAllocPolicy>;
// Keyed on the address inside the embedded image, not on the code object:
// every zone wraps the same static bytes in its own collectable object, so
// a wrapper key would both duplicate per zone and dangle once a zone dies.
using CodeMap =
    js::HashMap<const uint8_t*, uint32_t, js::DefaultHasher<const uint8_t*>,
                js::SystemAllocPolicy>;
using ShapeSet =
    js::HashSet<uint32_t, js::DefaultHasher<uint32_t>, js::SystemAllocPolicy>;
using ShapeCountMap =
    js::HashMap<uint32_t, uint64_t, js::DefaultHasher<uint32_t>,
                js::SystemAllocPolicy>;

struct MissedShapeIR {
  uint8_t cacheKind = 0;
  std::vector<uint8_t> cacheIR;
};
using MissedIRMap =
    js::HashMap<uint32_t, MissedShapeIR, js::DefaultHasher<uint32_t>,
                js::SystemAllocPolicy>;

struct State {
  std::mutex mu;
  uint32_t baselineCorpusSize = 0;
  uint32_t icCorpusSize = 0;

  uint64_t baselineAOTHits = 0;
  uint64_t baselineCompiles = 0;
  uint64_t icAOTHits = 0;
  uint64_t icZoneHits = 0;
  uint64_t icCompiles = 0;

  BaselineBlobMap blFuncs;
  ICBlobMap icStubs;
  CodeMap codeToStubBlob;
  ShapeSet shapesAOT;
  ShapeSet shapesOther;
  // Per-missed-shape attach-request count: zone-hit + compiled events for
  // the same shape hash. Companion to shapesOther; the set gives coverage
  // ratios, the map lets the reducer rank misses by how often each shape
  // actually earned an attach.
  ShapeCountMap otherShapeAttaches;
  // Snapshots the CacheIR of each miss the first time it's seen. Zone-hit
  // shapes never enter here because their stub body was compiled earlier
  // in this process; only compiler runs contribute an entry.
  MissedIRMap missedIR;
  std::string outPath;
};

std::atomic<bool> gEnabled{false};
std::atomic<bool> gInitialized{false};
std::mutex gInitMu;
std::atomic<State*> gState{nullptr};

State* CoverageState() { return gState.load(std::memory_order_acquire); }

class FileWriteFunc final : public mozilla::JSONWriteFunc {
 public:
  explicit FileWriteFunc(FILE* f) : mFile(f) {}
  void Write(const mozilla::Span<const char>& aStr) override {
    fwrite(aStr.Elements(), 1, aStr.Length(), mFile);
  }

 private:
  FILE* mFile;
};

}  // namespace

void AOTCoverage::EnsureInit(const AOTImage* image) {
  if (gInitialized.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(gInitMu);
  if (gInitialized.load(std::memory_order_relaxed)) return;

  const char* path = getenv("JS_AOT_COVERAGE_OUT");
  if (!path || !*path) {
    gInitialized.store(true, std::memory_order_release);
    return;
  }

  State* s = new State();
  s->outPath = path;

  if (image) {
    for (uint32_t i = 0; i < image->blobCount(); ++i) {
      AOTBlobReader r = image->blobAt(i);
      if (r.kind() == AOTBlobKind::BaselineFunction) {
        s->baselineCorpusSize++;
      } else if (r.kind() == AOTBlobKind::InlineCacheStub) {
        s->icCorpusSize++;
      }
    }
  }

  gState.store(s, std::memory_order_release);
  gEnabled.store(true, std::memory_order_release);
  gInitialized.store(true, std::memory_order_release);
}

bool AOTCoverage::IsEnabled() {
  return gEnabled.load(std::memory_order_relaxed);
}

void AOTCoverage::NoteBaselineInstalled(uint32_t blobIdx, bool selfHosted) {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  s->baselineAOTHits++;
  auto p = s->blFuncs.lookupForAdd(blobIdx);
  if (!p) {
    (void)s->blFuncs.add(p, blobIdx, BaselineBlobCounts{selfHosted, 1});
  } else {
    p->value().installs++;
    if (selfHosted) p->value().selfHosted = true;
  }
}

void AOTCoverage::NoteBaselineCompiled() {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  s->baselineCompiles++;
}

void AOTCoverage::NoteICStubLoaded(JitCode* code, uint32_t blobIdx) {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  (void)s->codeToStubBlob.put(code->raw(), blobIdx);
  auto p = s->icStubs.lookupForAdd(blobIdx);
  if (!p) {
    (void)s->icStubs.add(p, blobIdx, 0);
  }
}

void AOTCoverage::NoteICRequestAOTHit(JitCode* code, uint32_t shapeHash) {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  s->icAOTHits++;
  (void)s->shapesAOT.put(shapeHash);

  auto cp = s->codeToStubBlob.lookup(code->raw());
  if (!cp) return;
  auto p = s->icStubs.lookupForAdd(cp->value());
  if (!p) {
    (void)s->icStubs.add(p, cp->value(), 1);
  } else {
    p->value()++;
  }
}

void AOTCoverage::NoteICRequestZoneHit(uint32_t shapeHash) {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  s->icZoneHits++;
  (void)s->shapesOther.put(shapeHash);
  auto p = s->otherShapeAttaches.lookupForAdd(shapeHash);
  if (p) {
    p->value()++;
  } else {
    (void)s->otherShapeAttaches.add(p, shapeHash, uint64_t(1));
  }
}

void AOTCoverage::NoteICRequestCompiled(uint32_t shapeHash, uint8_t cacheKind,
                                        const uint8_t* cacheIR,
                                        uint32_t cacheIRLen) {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  s->icCompiles++;
  (void)s->shapesOther.put(shapeHash);
  {
    auto p = s->otherShapeAttaches.lookupForAdd(shapeHash);
    if (p) {
      p->value()++;
    } else {
      (void)s->otherShapeAttaches.add(p, shapeHash, uint64_t(1));
    }
  }

  if (!cacheIR || cacheIRLen == 0) return;
  auto p = s->missedIR.lookupForAdd(shapeHash);
  if (p) return;  // first sighting wins; subsequent misses on same shape are identical
  MissedShapeIR entry;
  entry.cacheKind = cacheKind;
  entry.cacheIR.assign(cacheIR, cacheIR + cacheIRLen);
  (void)s->missedIR.add(p, shapeHash, std::move(entry));
}

namespace {

template <size_t N>
void WriteShapeSet(mozilla::JSONWriter& w, const char (&name)[N],
                   const ShapeSet& set) {
  w.StartArrayProperty(name, mozilla::JSONWriter::SingleLineStyle);
  for (auto r = set.iter(); !r.done(); r.next()) {
    w.IntElement(int64_t(r.get()));
  }
  w.EndArray();
}

void WriteJson(State* s) {
  std::string finalPath = s->outPath;
  finalPath += ".";
  finalPath += std::to_string(int(getpid()));

  FILE* f = fopen(finalPath.c_str(), "w");
  if (!f) return;

  mozilla::JSONWriter w(mozilla::MakeUnique<FileWriteFunc>(f));
  w.Start();
  w.IntProperty("pid", int(getpid()));

  w.StartObjectProperty("corpus");
  w.IntProperty("baseline_functions", int64_t(s->baselineCorpusSize));
  w.IntProperty("ic_stubs", int64_t(s->icCorpusSize));
  w.EndObject();

  w.StartObjectProperty("requests");
  w.StartObjectProperty("baseline_functions");
  w.IntProperty("aot_hit", int64_t(s->baselineAOTHits));
  w.IntProperty("compiled", int64_t(s->baselineCompiles));
  w.EndObject();
  w.StartObjectProperty("ic_stubs");
  w.IntProperty("aot_hit", int64_t(s->icAOTHits));
  w.IntProperty("zone_cache_hit", int64_t(s->icZoneHits));
  w.IntProperty("compiled", int64_t(s->icCompiles));
  w.EndObject();
  w.EndObject();

  w.StartArrayProperty("baseline_functions");
  for (auto r = s->blFuncs.iter(); !r.done(); r.next()) {
    w.StartObjectElement(mozilla::JSONWriter::SingleLineStyle);
    w.IntProperty("blob", int64_t(r.get().key()));
    w.BoolProperty("self_hosted", r.get().value().selfHosted);
    w.IntProperty("installs", int64_t(r.get().value().installs));
    w.EndObject();
  }
  w.EndArray();

  w.StartArrayProperty("ic_stubs");
  for (auto r = s->icStubs.iter(); !r.done(); r.next()) {
    w.StartObjectElement(mozilla::JSONWriter::SingleLineStyle);
    w.IntProperty("blob", int64_t(r.get().key()));
    w.IntProperty("attaches", int64_t(r.get().value()));
    w.EndObject();
  }
  w.EndArray();

  WriteShapeSet(w, "ic_shapes_aot", s->shapesAOT);
  WriteShapeSet(w, "ic_shapes_other", s->shapesOther);

  // Per-missed-shape attach-request count. Reducer folds these into the
  // per-body summary so misses can be ranked by how heavily they were
  // requested vs. how many stub bodies they represent.
  w.StartArrayProperty("ic_shapes_other_attaches");
  for (auto r = s->otherShapeAttaches.iter(); !r.done(); r.next()) {
    w.StartObjectElement(mozilla::JSONWriter::SingleLineStyle);
    w.IntProperty("shape_hash", int64_t(r.get().key()));
    w.IntProperty("attach_count", int64_t(r.get().value()));
    w.EndObject();
  }
  w.EndArray();

  // Per-missed-shape CacheIR snapshot: hex-encoded byte stream plus the
  // cache kind. Reducer groups these by opcode-sequence to answer "which
  // stub bodies are missing from the corpus?".
  w.StartArrayProperty("ic_shapes_other_ir");
  for (auto r = s->missedIR.iter(); !r.done(); r.next()) {
    w.StartObjectElement(mozilla::JSONWriter::SingleLineStyle);
    w.IntProperty("shape_hash", int64_t(r.get().key()));
    w.IntProperty("cache_kind", int64_t(r.get().value().cacheKind));
    const auto& bytes = r.get().value().cacheIR;
    std::string hex;
    hex.reserve(bytes.size() * 2);
    static const char kHex[] = "0123456789abcdef";
    for (uint8_t b : bytes) {
      hex.push_back(kHex[(b >> 4) & 0xF]);
      hex.push_back(kHex[b & 0xF]);
    }
    w.StringProperty("ir_hex", mozilla::MakeStringSpan(hex.c_str()));
    w.EndObject();
  }
  w.EndArray();

  w.End();
  fclose(f);
}

}  // namespace

void AOTCoverage::FlushAndWrite() {
  State* s = CoverageState();
  if (!s) return;

  std::lock_guard<std::mutex> lock(s->mu);
  WriteJson(s);
}

}  // namespace js::jit

JS_PUBLIC_API void JS::FlushAOTCoverage() {
  js::jit::AOTCoverage::FlushAndWrite();
}

#endif  // ENABLE_JS_AOT
