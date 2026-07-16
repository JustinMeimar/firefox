/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOT.h"

#include "mozilla/Maybe.h"

#include <cstring>
#include <fstream>
#include <string>

#include "gc/Zone.h"
#include "jit/AOTBlobGenerated.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "js/BuildId.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"       // js::GetBuildId

#include "vm/Realm-inl.h"

namespace js::jit {

JitCode* AllocateAOTCode(JSContext* cx,
                               const AOTBlobDirectoryEntry* entry,
                               uint8_t* textBase, CodeKind codeKind) {
  mozilla::Maybe<AutoAllocInAtomsZone> az;
  if (!cx->zone() || !cx->zone()->isAtomsZone()) {
    az.emplace(cx);
  }

  if (!cx->zone()->ensureJitZoneExists(cx)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  uint8_t* codeStart = textBase + entry->codeOffset;
  JitCode* code = JitCode::NewStatic(cx, codeStart, entry->codeSize, codeKind);
  if (!code) {
    return nullptr;
  }

  return code;
}

mozilla::Maybe<AOTSlot> AOTIndirectionTable::findSlot(uintptr_t value) const {
  if (value == 0) return mozilla::Nothing();
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    if (slots_[i] == value) {
      return mozilla::Some(AOTSlot(i));
    }
  }
  return mozilla::Nothing();
}

void AOTIndirectionTable::dump() const {
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    JitSpew(JitSpew_BaselineAOT, "  slot[%u] %-30s = %p",
            i, AOTSlotName(AOTSlot(i)),
            (void*)get(AOTSlot(i)));
  }
}

AOTSlot AOTIndirectionTable::findSlotOrCrash(uintptr_t value) const {
  auto slot = findSlot(value);
  if (!slot) {
    JitSpew(JitSpew_BaselineAOT, "No AOT slot for %p", (void*)value);
    dump();
    MOZ_CRASH("No AOT slot for pointer");
  }
  return *slot;
}

AOTBlobWriter::AOTBlobWriter(AOTBlobKind kind, uint32_t nameHash,
                             std::string name)
    : kind_(kind), nameHash_(nameHash), name_(std::move(name)) {}

static bool ComputeAOTContainerFingerprint(
    Vector<uint8_t, 0, SystemAllocPolicy>& out);

static void WriteZero(std::ostream& out, size_t n) {
  static const char zeros[64] = {0};
  while (n > 0) {
    size_t chunk = std::min(n, sizeof(zeros));
    out.write(zeros, chunk);
    n -= chunk;
  }
}

static void WriteBytes(std::ostream& out, const void* p, size_t n) {
  if (n) out.write(reinterpret_cast<const char*>(p), n);
}

bool AOTContainerWriter::finalize(std::ostream& textOut,
                                  std::ostream& containerOut) {
  uint32_t blobCount = blobs_.length();
  if (blobCount == 0) return true;

  Vector<uint8_t, 0, SystemAllocPolicy> fingerprint;
  if (!ComputeAOTContainerFingerprint(fingerprint)) return false;
  uint32_t fpSize = uint32_t(fingerprint.length());

  Vector<AOTBlobDirectoryEntry, 2, SystemAllocPolicy> dirEntries;
  if (!dirEntries.reserve(blobCount)) return false;

  uint32_t textCursor = 0;
  for (uint32_t i = 0; i < blobCount; i++) {
    const auto& blob = blobs_[i];
    AOTBlobDirectoryEntry e{};
    e.kind = blob.kind();
    e.nameHash = blob.nameHash();
    e.codeSize = blob.codeBytes().size();
    e.fieldsSize = blob.fieldsBytes().size();
    e.arraysSize = blob.arraysBytes().size();

    textCursor = js::AlignBytes(textCursor, kAOTAlignment);
    e.codeOffset = textCursor;
    textCursor += e.codeSize;

    dirEntries.infallibleAppend(e);
  }

  uint32_t dirOffset =
      js::AlignBytes(sizeof(AOTContainerHeader) + fpSize, kAOTAlignment);
  uint32_t dirEnd =
      dirOffset + blobCount * sizeof(AOTBlobDirectoryEntry);
  uint32_t rodataDataStart = js::AlignBytes(dirEnd, kAOTAlignment);

  uint32_t dataCursor = rodataDataStart;
  for (uint32_t i = 0; i < blobCount; i++) {
    dirEntries[i].dataOffset = dataCursor;
    dataCursor += dirEntries[i].fieldsSize;
    dataCursor += dirEntries[i].arraysSize;
    dataCursor = js::AlignBytes(dataCursor, kAOTAlignment);
  }

  AOTContainerHeader hdr{};
  hdr.magic = AOT_CONTAINER_MAGIC;
  hdr.version = AOT_CONTAINER_VERSION;
  hdr.blobCount = blobCount;
  hdr.fingerprintSize = fpSize;

  {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < blobCount; i++) {
      const auto& blob = blobs_[i];
      uint32_t codeOffset = dirEntries[i].codeOffset;
      if (codeOffset > pos) {
        WriteZero(textOut, codeOffset - pos);
        pos = codeOffset;
      }
      auto codeBytes = blob.codeBytes();
      WriteBytes(textOut, codeBytes.data(), codeBytes.size());
      pos += codeBytes.size();
    }
  }

  {
    WriteBytes(containerOut, &hdr, sizeof(hdr));
    uint32_t pos = sizeof(hdr);
    if (fpSize > 0) {
      WriteBytes(containerOut, fingerprint.begin(), fpSize);
      pos += fpSize;
    }
    if (dirOffset > pos) {
      WriteZero(containerOut, dirOffset - pos);
      pos = dirOffset;
    }
    for (uint32_t i = 0; i < blobCount; i++) {
      WriteBytes(containerOut, &dirEntries[i], sizeof(AOTBlobDirectoryEntry));
      pos += sizeof(AOTBlobDirectoryEntry);
    }
    if (rodataDataStart > pos) {
      WriteZero(containerOut, rodataDataStart - pos);
      pos = rodataDataStart;
    }
    for (uint32_t i = 0; i < blobCount; i++) {
      const auto& blob = blobs_[i];
      auto fieldsBytes = blob.fieldsBytes();
      auto arraysBytes = blob.arraysBytes();
      WriteBytes(containerOut, fieldsBytes.data(), fieldsBytes.size());
      WriteBytes(containerOut, arraysBytes.data(), arraysBytes.size());
      pos += fieldsBytes.size() + arraysBytes.size();
      if (i + 1 < blobCount) {
        uint32_t aligned = js::AlignBytes(pos, kAOTAlignment);
        if (aligned > pos) {
          WriteZero(containerOut, aligned - pos);
          pos = aligned;
        }
      }
    }
  }
  return true;
}

static AOTCodegenOptions SnapshotAOTCodegenOptions() {
  AOTCodegenOptions fp;
  fp.disableInlining = JitOptions.disableInlining ? 1 : 0;
  fp.spectreObjectMitigations =
      JitOptions.spectreObjectMitigations ? 1 : 0;
  fp.spectreStringMitigations =
      JitOptions.spectreStringMitigations ? 1 : 0;
  fp.baselineBatching = JitOptions.baselineBatching ? 1 : 0;
  fp.baselineJitWarmUpThreshold = JitOptions.baselineJitWarmUpThreshold;
  fp.baselineQueueCapacity = JitOptions.baselineQueueCapacity;
  fp.trialInliningWarmUpThreshold =
      JitOptions.trialInliningWarmUpThreshold;
  return fp;
}

// Layout of the container fingerprint region (immediately after
// AOTContainerHeader):
//   [u32]        version (AOT_CONTAINER_VERSION)
//   [u32]        buildIdLen
//   [buildIdLen] embedder build ID bytes (js::GetBuildId)
//   [struct]     AOTCodegenOptions
//
// Prototype scope: CPU features are implicit in the shell binary that
// carries the AOT blob, so no separate CPU fingerprint is stored.
static bool ComputeAOTContainerFingerprint(
    Vector<uint8_t, 0, SystemAllocPolicy>& out) {
  JS::BuildIdCharVector buildId;
  if (!js::GetBuildId || !js::GetBuildId(&buildId)) {
    return false;
  }

  uint32_t version = AOT_CONTAINER_VERSION;
  uint32_t buildIdLen = uint32_t(buildId.length());
  AOTCodegenOptions opts = SnapshotAOTCodegenOptions();

  auto appendBytes = [&](const void* p, size_t n) {
    return out.append(reinterpret_cast<const uint8_t*>(p), n);
  };

  if (!appendBytes(&version, sizeof(version))) return false;
  if (!appendBytes(&buildIdLen, sizeof(buildIdLen))) return false;
  if (buildIdLen && !appendBytes(buildId.begin(), buildIdLen)) {
    return false;
  }
  if (!appendBytes(&opts, sizeof(opts))) return false;
  return true;
}

static bool VerifyContainerHeaderFingerprint(
    const AOTContainerHeader* hdr, const uint8_t* containerBase) {
  const uint8_t* stored = containerBase + sizeof(AOTContainerHeader);
  Vector<uint8_t, 0, SystemAllocPolicy> live;
  if (!ComputeAOTContainerFingerprint(live)) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT container: failed to compute live fingerprint");
    return false;
  }
  if (hdr->fingerprintSize != live.length() ||
      memcmp(stored, live.begin(), live.length()) != 0) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT container fingerprint mismatch (stored=%u live=%zu), "
            "AOT disabled",
            hdr->fingerprintSize, live.length());
    return false;
  }
  return true;
}

/* static */
mozilla::Maybe<AOTContainerReader> AOTContainerReader::fromEmbedded() {
  static mozilla::Maybe<bool> sFingerprintOK;
  static mozilla::Maybe<AOTContainerReader::ProbeSet> sBaselineProbes;

  if (GetAOTContainerSize() < sizeof(AOTContainerHeader)) {
    return mozilla::Nothing();
  }

  const auto* hdr = reinterpret_cast<const AOTContainerHeader*>(
      GetAOTContainer());
  if (hdr->magic != AOT_CONTAINER_MAGIC ||
      hdr->version != AOT_CONTAINER_VERSION) {
    return mozilla::Nothing();
  }

  if (!sFingerprintOK) {
    sFingerprintOK = mozilla::Some(
        VerifyContainerHeaderFingerprint(hdr, GetAOTContainer()));
  }
  if (!*sFingerprintOK) {
    return mozilla::Nothing();
  }

  const auto* dir = reinterpret_cast<const AOTBlobDirectoryEntry*>(
      GetAOTContainer() + AOTBlobDirectoryOffset(hdr));

  if (!sBaselineProbes) {
    AOTContainerReader::ProbeSet probes;
    for (uint32_t i = 0; i < hdr->blobCount; i++) {
      if (dir[i].kind != AOTBlobKind::BaselineFunction) continue;
      if (dir[i].codeSize == 0 && dir[i].fieldsSize == 0 &&
          dir[i].arraysSize == 0) {
        continue;
      }
      (void)probes.put(dir[i].nameHash);
    }
    sBaselineProbes = mozilla::Some(std::move(probes));
  }

  return mozilla::Some(AOTContainerReader(
      dir, GetAOTContainer(), GetAOTTextBase(), hdr->blobCount,
      sBaselineProbes.ptr()));
}

mozilla::Maybe<AOTBlobReader> AOTContainerReader::getBlob(
    AOTBlobKind kind, uint32_t nameHash) const {
  mozilla::Maybe<AOTBlobReader> found;
  anyBlob(kind, [&](AOTBlobReader& reader) {
    if (nameHash != 0 && reader.entry()->nameHash != nameHash) return false;
    found.emplace(reader);
    return true;
  });
  return found;
}

}  // namespace js::jit
