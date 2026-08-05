/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "JitInstrReporter.h"

#include "jit/Instr.h"
#include "jit/InstrSnapshot.h"
#include "mozilla/Atomics.h"
#include "nsIMemoryReporter.h"
#include "xpcprivate.h"

// [SMDOC] Phase-3 instrumentation memory reporter
//
// Phase-3 needs cross-process synchronized snapshots of the JS engine
// artifact stream. Rather than roll our own signal/pipe/interrupt
// infrastructure (which fights Firefox's fork server, per-thread
// signal masks, and seccomp sandbox), we register as a
// nsIMemoryReporter and piggyback on Firefox's built-in
// cross-process memory-report collection.
//
// When any of the following fires -- about:memory's "Measure" button,
// `ChromeUtils.requestMemoryReport`, `dumpMemoryInfoToTempDir`,
// or `nsIMemoryReporterManager::getReports` -- Firefox fans out to
// every process, each process's `CollectReports` runs on the main
// thread at a natural JS safe point, and we take our snapshot there.
// No signals, no reader threads, no fork races, and the whole path is
// sandbox-clean because it uses the same mechanism the browser itself
// uses.
//
// The snapshot writes to the process's own JSONL log (opened by
// JSInstr::Init at first JitRuntime spin-up). It is a *side effect*
// of the reporter call, not visible in about:memory output. We also
// emit a handful of `KIND_OTHER` counters to about:memory so someone
// looking at the memory dump sees at a glance that instrumentation is
// active and what the live totals are.

namespace mozilla {

NS_IMPL_ISUPPORTS(JitInstrReporter, nsIMemoryReporter)

/* static */
already_AddRefed<JitInstrReporter> JitInstrReporter::Create() {
  return MakeAndAddRef<JitInstrReporter>();
}

/* static */
void JitInstrReporter::Register() {
  RegisterStrongMemoryReporter(Create());
}

NS_IMETHODIMP
JitInstrReporter::CollectReports(nsIHandleReportCallback* aHandleReport,
                                 nsISupports* aData, bool aAnonymize) {
  using namespace js::jit;

  // Skip if instrumentation is disabled in this process. Non-JS
  // helper processes (crashhelper, GPU, RDD, socket, ...) never
  // opened a JSONL log so JSInstr::Enabled returns false and we do
  // nothing here -- exactly the behaviour we want.
  if (!JSInstr::Enabled(InstrCh_Snapshot)) {
    return NS_OK;
  }

  // Every reporter call gets a fresh sequence number. A harness can write a
  // coordinated marker to `$JS_INSTR_DIR/marker.txt` before triggering the
  // report so every process emits the same snapshot identity.
  static Atomic<uint32_t> sSeq{0};
  uint32_t seq = ++sSeq;

  char marker[256];
  bool haveMarker = false;
  if (const char* dir = getenv("JS_INSTR_DIR")) {
    char path[2048];
    int r = snprintf(path, sizeof(path), "%s/marker.txt", dir);
    if (r > 0 && size_t(r) < sizeof(path)) {
      if (FILE* f = fopen(path, "r")) {
        if (fgets(marker, sizeof(marker), f)) {
          size_t l = strlen(marker);
          while (l && (marker[l - 1] == '\n' || marker[l - 1] == '\r' ||
                       marker[l - 1] == ' ')) {
            marker[--l] = 0;
          }
          haveMarker = l > 0;
        }
        fclose(f);
      }
    }
  }
  if (!haveMarker) {
    snprintf(marker, sizeof(marker), "measure-%u", seq);
  }

  // Grab the main-thread JSContext to walk zones for entries-flush.
  // If this process has no JS context (a pure XPCOM helper), skip the
  // snapshot walk but still report the KIND_OTHER counters below.
  JSContext* cx = nullptr;
  if (XPCJSContext* xpcCx = XPCJSContext::Get()) {
    cx = xpcCx->Context();
  }
  if (cx) {
    InstrSnapshot::Now(cx, marker);
  }

  // Publish live totals to about:memory so anyone browsing the dump
  // can confirm instrumentation is active. These duplicate values in
  // the JSONL's snapshot-live event; they exist here only for
  // human-visible sanity.
  JSInstr::LiveCounters c{};
  JSInstr::GetLiveCounters(&c);
  aHandleReport->Callback(
      ""_ns, "explicit/js-instrumentation/live-mmap-bytes"_ns,
      nsIMemoryReporter::KIND_NONHEAP, nsIMemoryReporter::UNITS_BYTES,
      int64_t(c.liveMmapBytes),
      "Total bytes mmapped for executable pools (Phase-3 instrumentation)."_ns,
      aData);
  aHandleReport->Callback(
      ""_ns, "explicit/js-instrumentation/live-ic-body-bytes"_ns,
      nsIMemoryReporter::KIND_NONHEAP, nsIMemoryReporter::UNITS_BYTES,
      int64_t(c.liveIcBodyBytes),
      "Total bytes of unique CacheIR body code interned so far."_ns, aData);
  for (size_t i = 0; i < 8; ++i) {
    if (c.perOwner[i].codeBytes == 0) continue;
    nsCString path("explicit/js-instrumentation/by-owner/");
    path.Append(js::jit::Name(c.perOwner[i].owner));
    aHandleReport->Callback(
        ""_ns, path, nsIMemoryReporter::KIND_NONHEAP,
        nsIMemoryReporter::UNITS_BYTES, int64_t(c.perOwner[i].codeBytes),
        "Live JitCode bytes owned by this artifact class."_ns, aData);
  }
  return NS_OK;
}

}  // namespace mozilla
