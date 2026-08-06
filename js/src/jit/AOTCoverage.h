/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTCoverage_h
#define jit_AOTCoverage_h

#ifdef ENABLE_JS_AOT

#  include <cstdint>

namespace js::jit {

class JitCode;
class AOTImage;

// [SMDOC] AOT Coverage
// ====================
//
// Per-process accounting of how much of the embedded AOT image the workload
// actually consumed, for two artifact populations: baseline function bodies
// and baseline CacheIR stubs.
//
// Everything is counted at request time, never by sampling live heap state.
// A stub's execution counter is zeroed whenever a new stub joins its chain
// and the stub itself is freed on any GC-driven purge or script teardown,
// so a shutdown walk measures survivorship rather than lifetime use and
// undercounts by orders of magnitude. Request-event counting is immune to
// all of that.
//
// Exactly one outcome hook fires per stub-code request, which makes the
// three outcomes a partition and gives the coverage ratio a real
// denominator:
//
//   aot hit        the image supplied the code
//   zone hit       already compiled in this zone, image did not have it
//   compiled       compiler ran
//
// Two coverage directions fall out, and they answer different questions:
//
//   corpus utilization  used blobs / corpus size. How much of what we
//                       shipped earned its bytes.
//   workload coverage   distinct shapes served by the image / distinct
//                       shapes the workload asked for. How well a corpus
//                       recorded elsewhere generalizes to this workload.
//
// Shapes are identified by a content hash over the CacheIR bytes, which is
// free of addresses and therefore comparable across processes. The reducer
// unions the per-process shape sets before taking the ratio.
//
// Activation is gated on the JS_AOT_COVERAGE_OUT env var at init time. When
// unset every hook is one relaxed atomic load returning false.
//
// Output is one JSON file per process at $JS_AOT_COVERAGE_OUT.<pid>,
// rewritten in full on each flush so the last flush to run wins.

class AOTCoverage {
 public:
  static void EnsureInit(const AOTImage* image);

  static bool IsEnabled();

  static void NoteBaselineInstalled(uint32_t blobIdx, bool selfHosted);
  static void NoteBaselineCompiled();

  // Called once per corpus stub that reaches a zone's stub code table, to
  // map the shared code back to its blob at request time.
  static void NoteICStubLoaded(JitCode* code, uint32_t blobIdx);

  static void NoteICRequestAOTHit(JitCode* code, uint32_t shapeHash);
  static void NoteICRequestZoneHit(uint32_t shapeHash);
  static void NoteICRequestCompiled(uint32_t shapeHash);

  static void FlushAndWrite();
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTCoverage_h
