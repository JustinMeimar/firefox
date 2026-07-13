/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOT.h"

#include "mozilla/Maybe.h"
#include "mozilla/Sprintf.h"

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
                             uint32_t corpusIndex, std::string name)
    : kind_(kind),
      nameHash_(nameHash),
      corpusIndex_(corpusIndex),
      name_(std::move(name)) {}

static void emitAsmBytes(std::ostream& out, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i += 16) {
    size_t end = std::min(i + 16, len);
    out << "    .byte ";
    for (size_t j = i; j < end; j++) {
      char buf[8];
      SprintfLiteral(buf, "%s0x%02x", j > i ? ", " : "", unsigned(data[j]));
      out << buf;
    }
    out << "\n";
  }
}

// Blob display names come from JS atoms (e.g. "Scheduler.prototype.run",
// "$RegExpFlagsGetter", "set") and are not safe to embed verbatim as
// assembler symbols: `.` / `$` are fragile in GAS, and duplicate display
// names collide (219 distinct `GameBoyCore.prototype.OPCODE` scripts in a
// single corpus). Labels here are cosmetic — the loader consumes the
// container header + directory, not per-blob symbols — so we prefix with
// the blob index for uniqueness and sanitize the tail for readability.
static std::string SanitizeBlobLabel(const std::string& name, uint32_t index) {
  std::string out;
  out.reserve(name.size() + 12);
  char idxBuf[16];
  SprintfLiteral(idxBuf, "%u_", index);
  out += idxBuf;
  for (char c : name) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_') {
      out += c;
    } else {
      out += '_';
    }
  }
  return out;
}

static void emitAsmZeroPadding(std::ostream& out, size_t len) {
  if (len > 0) {
    out << "    .zero " << len << "\n";
  }
}

static bool ComputeAOTContainerFingerprint(
    Vector<uint8_t, 0, SystemAllocPolicy>& out);

bool AOTContainerWriter::finalize(std::ostream& out) {
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
    e.corpusIndex = blob.corpusIndex();
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

  out << "// AOT Baseline - generated by --aot-dump-baseline\n";
  out << "// " << blobCount << " blob(s)\n";

  out << ".section .text.aot,\"ax\",@progbits\n";
  out << ".balign " << kAOTAlignment << "\n";
  out << ".global bl_aot_text_start\n";
  out << ".global bl_aot_text_end\n";
  out << "bl_aot_text_start:\n";

  for (uint32_t i = 0; i < blobCount; i++) {
    const auto& blob = blobs_[i];

    uint32_t padBefore = dirEntries[i].codeOffset -
        (i == 0 ? 0 : (dirEntries[i-1].codeOffset + dirEntries[i-1].codeSize));
    if (padBefore > 0) {
      emitAsmZeroPadding(out, padBefore);
    }

    std::string label = SanitizeBlobLabel(blob.name(), i);
    out << "// --- Code blob " << i << " " << blob.name() << " ---\n";
    out << "bl_aot_" << label << "_code:\n";
    auto codeBytes = blob.codeBytes();
    if (!codeBytes.empty()) {
      emitAsmBytes(out, codeBytes.data(), codeBytes.size());
    }
  }

  out << "bl_aot_text_end:\n";

  out << ".section .rodata\n";
  out << ".balign " << kAOTAlignment << "\n";
  out << ".global bl_aot_container_start\n";
  out << ".global bl_aot_container_end\n";
  out << "bl_aot_container_start:\n";

  out << "// --- Container Header ---\n";
  emitAsmBytes(out, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));

  if (fpSize > 0) {
    out << "// --- Container Fingerprint (" << fpSize << " bytes) ---\n";
    emitAsmBytes(out, fingerprint.begin(), fpSize);
  }

  size_t padToDir = dirOffset - (sizeof(AOTContainerHeader) + fpSize);
  if (padToDir > 0) {
    out << "// --- Padding to directory ---\n";
    emitAsmZeroPadding(out, padToDir);
  }

  for (uint32_t i = 0; i < blobCount; i++) {
    out << "// --- Directory Entry " << i << " (kind="
        << static_cast<uint32_t>(dirEntries[i].kind) << ") ---\n";
    emitAsmBytes(out, reinterpret_cast<const uint8_t*>(&dirEntries[i]),
                 sizeof(AOTBlobDirectoryEntry));
  }

  size_t padSize = rodataDataStart - dirEnd;
  if (padSize > 0) {
    out << "// --- Alignment padding ---\n";
    emitAsmZeroPadding(out, padSize);
  }

  for (uint32_t i = 0; i < blobCount; i++) {
    const auto& blob = blobs_[i];

    std::string label = SanitizeBlobLabel(blob.name(), i);
    out << "// --- Fields/Arrays " << i << " " << blob.name() << " ---\n";

    out << "bl_aot_" << label << "_fields:\n";
    auto fieldsBytes = blob.fieldsBytes();
    if (!fieldsBytes.empty()) {
      emitAsmBytes(out, fieldsBytes.data(), fieldsBytes.size());
    }

    out << "bl_aot_" << label << "_arrays:\n";
    auto arraysBytes = blob.arraysBytes();
    if (!arraysBytes.empty()) {
      emitAsmBytes(out, arraysBytes.data(), arraysBytes.size());
    }

    if (i + 1 < blobCount) {
      uint32_t curPos = dirEntries[i].dataOffset + dirEntries[i].fieldsSize +
                        arraysBytes.size();
      uint32_t nextAligned = js::AlignBytes(curPos, kAOTAlignment);
      if (nextAligned > curPos) {
        out << "// --- Inter-blob padding ---\n";
        emitAsmZeroPadding(out, nextAligned - curPos);
      }
    }
  }

  out << "bl_aot_container_end:\n";
  out << ".section .note.GNU-stack,\"\",@progbits\n";
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
