/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>

namespace js::jit {

extern "C" {
  extern const uint8_t baseline_blob_start[];
  extern const uint8_t baseline_blob_end[];
}
inline uint8_t*
GetAOTBaselineBlob() { return const_cast<uint8_t*>(baseline_blob_start); }

inline std::size_t
GetAOTBaselineSize() { return baseline_blob_end - baseline_blob_start; }

// Stable indices for metadata values in the manifest.
enum class BaselineMetadataID : uint32_t {
  // Scalar offsets (entry points)
  InterpretOp = 0,
  InterpretOpNoDebugTrap,
  BailoutPrologue,
  ProfilerEnterToggle,
  ProfilerExitToggle,
  DebugTrapHandler,
  DispatchTableOffset,

  // CallVM offsets
  CallVMDebugPrologue,
  CallVMDebugEpilogue,
  CallVMDebugAfterYield,

  // Vector counts (for loader to reconstruct vectors)
  DebugInstrumentationCount,
  DebugTrapCount,
  CodeCoverageCount,
  ICReturnCount,
  PatchCount,  // Number of patch entries

  Count  // Total entries (now 15)
};

// Patch types for runtime pointer relocation
enum class PatchType : uint32_t {
  DispatchTable = 0,        // Load instructions that reference dispatch table base
  WellKnownSymbols,         // Runtime well-known symbols pointer (future)
  DebugTrapHandler,         // Debug trap handler JitCode pointer (future)
  VMWrapper,                // VM function wrapper addresses (future)
  // Future: TraceLoggerState, etc.
};

// Patch entry for runtime pointer relocation (12 bytes)
struct alignas(4) PatchEntry {
  uint32_t offset;       // Offset in machine code to patch
  PatchType type;        // What runtime value to patch with
  uint32_t aux;          // Auxiliary data (e.g., VMFunction ID, opcode index)
};
static_assert(sizeof(PatchEntry) == 12, "PatchEntry must be 12 bytes");

static const uint32_t AOT_BASELINE_FOOTER_MAGIC = 0x424C494E;
struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_BASELINE_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1; // Format version
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

// Todo: Replace staic_asserts with MOZ_ASSERTS.
static_assert(sizeof(BaselineAOTFooter) == 12, "Footer must be 12 bytes");

// Manifest header with fixed-size metadata array.
// Variable-length payloads follow this structure in the binary.
struct alignas(4) BaselineManifest {
  uint32_t metadata[uint32_t(BaselineMetadataID::Count)];

  // Followed by:
  // - uint32_t debugInstrumentation[DebugInstrumentationCount]
  // - uint32_t debugTraps[DebugTrapCount]
  // - uint32_t codeCoverage[CodeCoverageCount]
  // - ICReturnOffsetEntry icReturns[ICReturnCount]
  // - PatchEntry patches[PatchCount]
};
static_assert(sizeof(BaselineManifest) == 60, "Manifest must be 60 bytes (15 fields × 4)");

// IC return offset entry (8 bytes).
struct alignas(4) ICReturnOffsetEntry {
  uint32_t offset;
  uint32_t opcode;  // JSOp as uint32_t
};
static_assert(sizeof(ICReturnOffsetEntry) == 8, "ICReturnOffsetEntry must be 8 bytes");

}  // namespace js::jit


#endif  // jit_BaselineAOT_h
