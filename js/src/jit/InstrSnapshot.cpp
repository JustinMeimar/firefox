/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/InstrSnapshot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gc/PublicIterators.h"
#include "gc/Zone.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/Instr.h"
#include "jit/JitScript.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"

#include "gc/GC-inl.h"
#include "vm/JSScript-inl.h"

namespace js::jit {

namespace {

struct PoolCopy {
  uint32_t poolId;
  const char* poolKind;
  void* base;
  size_t mmapBytes;
  size_t usedBytes;
};

using PoolCopyVec = Vector<PoolCopy, 32, SystemAllocPolicy>;

void CollectPool(void* userdata, const JSInstr::PoolInfo& info) {
  auto* v = static_cast<PoolCopyVec*>(userdata);
  (void)v->append(PoolCopy{info.poolId, info.poolKind, info.base,
                           info.mmapBytes, info.usedBytes});
}

// Parses a "Name: value kB" style line from smaps into an unsigned int.
// Returns 0 on parse failure.
uint64_t ParseKb(const char* line, const char* key) {
  size_t klen = strlen(key);
  if (strncmp(line, key, klen) != 0) return 0;
  const char* p = line + klen;
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  return strtoull(p, nullptr, 10);
}

// One in-progress smaps entry (header + per-key values).
struct SmapsEntry {
  uint64_t start = 0;
  uint64_t end = 0;
  char perms[8] = {0};
  char path[256] = {0};
  uint64_t sizeKb = 0;
  uint64_t rssKb = 0;
  uint64_t pssKb = 0;
  uint64_t sharedCleanKb = 0;
  uint64_t sharedDirtyKb = 0;
  uint64_t privateCleanKb = 0;
  uint64_t privateDirtyKb = 0;
  uint64_t referencedKb = 0;
  uint64_t anonymousKb = 0;
  bool valid = false;
};

bool RangesOverlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
  return a0 < b1 && b0 < a1;
}

bool OverlapsAnyPool(const SmapsEntry& e, const PoolCopyVec& pools) {
  for (const PoolCopy& p : pools) {
    uint64_t p0 = uint64_t(uintptr_t(p.base));
    uint64_t p1 = p0 + uint64_t(p.mmapBytes);
    if (RangesOverlap(e.start, e.end, p0, p1)) return true;
  }
  return false;
}

void EmitSmapsEntry(const SmapsEntry& e) {
  JSInstr::SmapsRow r{e.start,
                      e.end,
                      e.sizeKb,
                      e.rssKb,
                      e.pssKb,
                      e.sharedCleanKb,
                      e.sharedDirtyKb,
                      e.privateCleanKb,
                      e.privateDirtyKb,
                      e.referencedKb,
                      e.anonymousKb,
                      e.perms,
                      e.path};
  JSInstr::LogSnapshotSmapsRow(r);
}

// Walk /proc/self/smaps once. Header lines look like:
//   7f7d84400000-7f7d84600000 rw-p 00000000 00:00 0            [heap]
// Each header is followed by "Key: value kB" lines until the next
// header. We keep the current entry open, populate per-key values, and
// on hitting a new header line (or EOF) decide whether to emit it based
// on overlap with any live pool range.
void EmitSmaps(const PoolCopyVec& pools) {
#ifdef XP_LINUX
  FILE* f = fopen("/proc/self/smaps", "r");
  if (!f) return;
  char line[4096];
  SmapsEntry cur;
  while (fgets(line, sizeof(line), f)) {
    // Header: contains '-' between two hex addresses and a space.
    uint64_t s = 0, e = 0;
    int permsOff = 0;
    if (sscanf(line, "%llx-%llx %n", (unsigned long long*)&s,
               (unsigned long long*)&e, &permsOff) == 2 &&
        permsOff > 0) {
      if (cur.valid && OverlapsAnyPool(cur, pools)) {
        EmitSmapsEntry(cur);
      }
      cur = SmapsEntry();
      cur.start = s;
      cur.end = e;
      const char* rest = line + permsOff;
      // Perms field: 4 chars.
      size_t i = 0;
      while (i < sizeof(cur.perms) - 1 && rest[i] && rest[i] != ' ') {
        cur.perms[i] = rest[i];
        ++i;
      }
      cur.perms[i] = 0;
      // Path is the last whitespace-separated field on the line, if
      // any.
      const char* p = strrchr(line, ' ');
      if (p && *(p + 1) != '\n' && *(p + 1) != 0) {
        size_t j = 0;
        ++p;
        while (j < sizeof(cur.path) - 1 && *p && *p != '\n') {
          cur.path[j++] = *p++;
        }
        cur.path[j] = 0;
      }
      cur.valid = true;
      continue;
    }
    if (!cur.valid) continue;
    if (uint64_t v = ParseKb(line, "Size"))
      cur.sizeKb = v;
    else if (uint64_t v = ParseKb(line, "Rss"))
      cur.rssKb = v;
    else if (uint64_t v = ParseKb(line, "Pss"))
      cur.pssKb = v;
    else if (uint64_t v = ParseKb(line, "Shared_Clean"))
      cur.sharedCleanKb = v;
    else if (uint64_t v = ParseKb(line, "Shared_Dirty"))
      cur.sharedDirtyKb = v;
    else if (uint64_t v = ParseKb(line, "Private_Clean"))
      cur.privateCleanKb = v;
    else if (uint64_t v = ParseKb(line, "Private_Dirty"))
      cur.privateDirtyKb = v;
    else if (uint64_t v = ParseKb(line, "Referenced"))
      cur.referencedKb = v;
    else if (uint64_t v = ParseKb(line, "Anonymous"))
      cur.anonymousKb = v;
  }
  if (cur.valid && OverlapsAnyPool(cur, pools)) {
    EmitSmapsEntry(cur);
  }
  fclose(f);
#else
  (void)pools;
#endif
}

}  // namespace

// Walks every BaseScript in every zone of `rt` and collects its
// ICScript::entryCount_ plus the live enteredCount of every attached
// IC stub (optimized chain + fallback). Must be called from the
// runtime's owning thread at a safe point.
//
// The two output vectors are flat and joined by (icEntryStart,
// icEntryCount): the harness reads each script row and takes an
// icEntryCount-long slice of the ic-row vector starting at
// icEntryStart. This keeps the on-disk representation compact and
// avoids per-script mallocs.
static void CollectAndEmitEntries(JSRuntime* rt, const char* reason) {
  if (!JSInstr::Enabled(InstrCh_Demand)) return;
  if (!rt) return;

  Vector<JSInstr::EntriesFlushRow, 128, SystemAllocPolicy> scripts;
  Vector<JSInstr::IcEntryRow, 512, SystemAllocPolicy> icRows;

  for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
    for (auto base = zone->cellIter<BaseScript>(); !base.done(); base.next()) {
      if (!base->hasJitScript()) continue;
      JSScript* script = base->asJSScript();
      JitScript* js = script->jitScript();
      ICScript* ic = js->icScript();
      if (!ic) continue;

      uint32_t scriptId = JSInstr::ScriptLocalId(script);
      uint32_t icStart = uint32_t(icRows.length());

      const uint32_t nEntries = ic->numICEntries();
      for (uint32_t i = 0; i < nEntries; ++i) {
        ICEntry& entry = ic->icEntry(i);
        ICFallbackStub* fallback = ic->fallbackStub(i);
        const uint32_t bcOffset = fallback->pcOffset();
        const uint32_t siteId = JSInstr::SiteLocalId(script, bcOffset);

        ICStub* s = entry.firstStub();
        while (s && !s->isFallback()) {
          ICCacheIRStub* cs = s->toCacheIRStub();
          const CacheIRStubInfo* info = cs->stubInfo();
          Sha1Digest bodyId =
              info ? Sha1(mozilla::Span<const uint8_t>(
                         reinterpret_cast<const uint8_t*>(info->code()),
                         info->codeLength()))
                   : Sha1Digest{};
          (void)icRows.append(JSInstr::IcEntryRow{
              siteId, bodyId, uint64_t(cs->enteredCount()), false});
          s = cs->next();
        }
        Sha1Digest fbId{};  // fallback shares site_id; zero digest as sentinel
        (void)icRows.append(JSInstr::IcEntryRow{
            siteId, fbId, uint64_t(fallback->enteredCount()), true});
      }

      uint32_t icCount = uint32_t(icRows.length() - icStart);
      (void)scripts.append(JSInstr::EntriesFlushRow{scriptId, ic->entryCount(),
                                                    icStart, icCount});
    }
  }

  JSInstr::LogEntriesFlush(JSInstr::RuntimeLocalId(rt), reason,
                           mozilla::Span<const JSInstr::EntriesFlushRow>(
                               scripts.begin(), scripts.length()),
                           mozilla::Span<const JSInstr::IcEntryRow>(
                               icRows.begin(), icRows.length()));
}

void InstrSnapshot::Now(JSContext* cx, const char* marker) {
  if (!JSInstr::Enabled(InstrCh_Snapshot)) return;

  JSInstr::LogSnapshotMarker(marker ? marker : "");

  PoolCopyVec pools;
  JSInstr::ForEachLivePool(&pools, &CollectPool);

  for (const PoolCopy& p : pools) {
    size_t unused = p.mmapBytes > p.usedBytes ? (p.mmapBytes - p.usedBytes) : 0;
    JSInstr::LogSnapshotFootprint(p.poolId, p.poolKind, p.mmapBytes,
                                  p.usedBytes, unused);
  }

  JSInstr::LiveCounters c{};
  JSInstr::GetLiveCounters(&c);
  JSInstr::LogSnapshotLive(c);

  if (cx) {
    CollectAndEmitEntries(cx->runtime(), "snapshot");
  }

  EmitSmaps(pools);
}

// Called from JSRuntime::destroyRuntime BEFORE gc/script teardown so
// every stub is still live and enteredCount is authoritative. Emits
// one entries-flush row per JitScript with each live IC stub's real
// execution count -- the coverage question's only sound weighting.
void InstrSnapshot::AtRuntimeShutdown(JSRuntime* rt) {
  if (!JSInstr::Enabled(InstrCh_Demand) || !rt) return;
  if (!JSInstr::MarkRuntimeEntriesFlushed(rt)) return;
  CollectAndEmitEntries(rt, "runtime-shutdown");
}

}  // namespace js::jit
