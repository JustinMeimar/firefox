/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InstrSnapshot_h
#define jit_InstrSnapshot_h

// [SMDOC] Phase-3 instrumentation snapshot
//
// A snapshot is a synchronized checkpoint in the log stream, produced
// on demand (SIGUSR1 or a shell builtin). It is a sequence of related
// events sharing an anchor:
//
//   snapshot-marker      one line, {marker: "<name>"}
//   snapshot-footprint   one line per live ExecutablePool with mincore
//                        residency and usedBytes/mmapBytes
//   snapshot-live        one line: total live counters per artifact
//                        class, sourced from InstrRegistry atomics
//   entries-flush        one line per registered runtime with the
//                        (script_local_id, entered_count) rows (D2)
//   snapshot-smaps       one line per /proc/self/smaps entry whose
//                        [start,end) overlaps a live pool (Linux only)
//
// Multi-process alignment is external: the harness writes a marker
// name to `$JS_INSTR_DIR/marker.txt`, then broadcasts SIGUSR1 to every
// participating process. Each process's signal thread reads the marker
// file, requests an interrupt on every registered runtime, and the
// interrupt callback runs `InstrSnapshot::Now` on the owning thread.

#include "js/TypeDecls.h"

namespace js::jit {

class InstrSnapshot {
 public:
  static void Now(JSContext* cx, const char* marker);
  // Emit a runtime-shutdown entries-flush. Safe to call from
  // JSRuntime::destroyRuntime; must run before scripts/GC teardown so
  // ICStub::enteredCount() is still authoritative.
  static void AtRuntimeShutdown(JSRuntime* rt);
};

}  // namespace js::jit

#endif  // jit_InstrSnapshot_h
