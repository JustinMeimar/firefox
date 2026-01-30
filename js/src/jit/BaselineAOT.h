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

inline uint8_t* GetAOTBaselineBlob() {
  return const_cast<uint8_t*>(baseline_blob_start);
}

inline std::size_t GetAOTBaselineSize() {
  return baseline_blob_end - baseline_blob_start;
}

// =============================================================================
// UNIFIED METADATA MACRO SYSTEM
// =============================================================================
//
// This is the single source of truth for all baseline AOT metadata fields.
// All enums, structs, and serialization/deserialization code are derived
// from these macros to eliminate manual synchronization.
//
// Parameters for offset fields: (ENUM_NAME, SERIALIZE_EXPR, LAYOUT_FIELD, INTERP_FIELD, CALLVM_FIELD)
//   - ENUM_NAME: Name in BaselineMetadataID enum
//   - SERIALIZE_EXPR: Expression to get value during serialization (from BaselineInterpreterGenerator)
//   - LAYOUT_FIELD: Field name in AOTBlobLayout (empty if not used)
//   - INTERP_FIELD: Field name in BaselineInterpreter (empty if not used)
//   - CALLVM_FIELD: Field name in callVMOffsets_ (empty if not CallVM-related)
//
// Parameters for count fields: (ENUM_NAME, SERIALIZE_EXPR, LAYOUT_FIELD)

#define FOR_EACH_BASELINE_OFFSET_FIELD(X) \
  X(InterpretOp, interpretOpOffset_, interpretOpOffset, interpretOpOffset_, ) \
  X(InterpretOpNoDebugTrap, interpretOpNoDebugTrapOffset_, , interpretOpNoDebugTrapOffset_, ) \
  X(BailoutPrologue, bailoutPrologueOffset_.offset(), , bailoutPrologueOffset_, ) \
  X(ProfilerEnterToggle, profilerEnterFrameToggleOffset_.offset(), , profilerEnterToggleOffset_, ) \
  X(ProfilerExitToggle, profilerExitFrameToggleOffset_.offset(), , profilerExitToggleOffset_, ) \
  X(DebugTrapHandler, debugTrapHandlerOffset_, , debugTrapHandlerOffset_, ) \
  X(DispatchTableOffset, tableOffset_, dispatchTableOffset, , ) \
  X(CallVMDebugPrologue, handler.callVMOffsets().debugPrologueOffset, , , debugPrologueOffset) \
  X(CallVMDebugEpilogue, handler.callVMOffsets().debugEpilogueOffset, , , debugEpilogueOffset) \
  X(CallVMDebugAfterYield, handler.callVMOffsets().debugAfterYieldOffset, , , debugAfterYieldOffset) \
  X(HeaderSize, code->headerSize(), , , ) \
  X(PrologueEndOffset, warmUpCheckPrologueOffset_.offset(), prologueEndOffset, , )

#define FOR_EACH_BASELINE_COUNT_FIELD(X) \
  X(DebugInstrumentationCount, handler.debugInstrumentationOffsets().length(), debugInstrCount) \
  X(DebugTrapCount, aotAccumulator_.debugTraps.length(), debugTrapCount) \
  X(CodeCoverageCount, handler.codeCoverageOffsets().length(), codeCoverageCount) \
  X(ICReturnCount, handler.icReturnOffsets().length(), icReturnCount) \
  X(PatchCount, aotAccumulator_.patches.length(), patchCount) \
  X(OpHandlerOffsetCount, opHandlerOffsets_.length(), opHandlerCount)

// Master macro that includes all metadata fields
#define FOR_EACH_BASELINE_METADATA_FIELD(X) \
  FOR_EACH_BASELINE_OFFSET_FIELD(X) \
  FOR_EACH_BASELINE_COUNT_FIELD(X)


// Auto-generated enum from the unified metadata macros.
// This eliminates manual synchronization between enum and field definitions.
enum class BaselineMetadataID : uint32_t {
#define ENUM_ENTRY_OFFSET(NAME, ...) NAME,
#define ENUM_ENTRY_COUNT(NAME, ...) NAME,
  FOR_EACH_BASELINE_OFFSET_FIELD(ENUM_ENTRY_OFFSET)
  FOR_EACH_BASELINE_COUNT_FIELD(ENUM_ENTRY_COUNT)
#undef ENUM_ENTRY_OFFSET
#undef ENUM_ENTRY_COUNT
  Count
};

// In order to perform the patch, the AOT loader must provide the
// code base address and the dispatch table offset.
struct PatchContext {
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;
    PatchContext(uint8_t* codeBase_, uint32_t dispatchTableOffset_)
      : codeBase(codeBase_), dispatchTableOffset(dispatchTableOffset_) {}
};

struct alignas(8) DispatchTablePatch {
    /// Offset from the blob to the start of the opcode handler (what the
    /// patch'ed pointer points to.)
    uint32_t handlerOffset;
    /// Index into the dispatch table 
    uint32_t dispatchTableIndex;
    DispatchTablePatch(uint32_t handlerOffset_, uint32_t dispatchTableIndex_)
      : handlerOffset(handlerOffset_), dispatchTableIndex(dispatchTableIndex_) {}
};

void applyPatch(const PatchContext& ctx, const DispatchTablePatch& entry);

static const uint32_t AOT_FOOTER_MAGIC = 0x424C494E;
struct alignas(4) BaselineAOTFooter {
  uint32_t magic = AOT_FOOTER_MAGIC; // 'BLIN'
  uint32_t version = 1;
  uint32_t manifestOffset = 0; // Absolute offset from blob start
};

// Auto-generated typed struct for baseline manifest.
// Provides type-safe named field access instead of array indexing.
struct alignas(4) BaselineManifest {
#define FIELD_DECL_OFFSET(NAME, ...) uint32_t NAME;
#define FIELD_DECL_COUNT(NAME, ...) uint32_t NAME;
  FOR_EACH_BASELINE_OFFSET_FIELD(FIELD_DECL_OFFSET)
  FOR_EACH_BASELINE_COUNT_FIELD(FIELD_DECL_COUNT)
#undef FIELD_DECL_OFFSET
#undef FIELD_DECL_COUNT
  // Followed by variable-length arrays:
  // uint32_t debugInstrumentation[DebugInstrumentationCount]
  // uint32_t debugTraps[DebugTrapCount]
  // uint32_t codeCoverage[CodeCoverageCount]
  // ICReturnOffsetEntry icReturns[ICReturnCount]
  // DispatchTablePatch patches[PatchCount]
  // uint32_t opHandlerOffsets[OpHandlerOffsetCount]
};

struct alignas(4) ICReturnOffsetEntry {
  uint32_t offset;
  uint32_t opcode;
};

static_assert(sizeof(BaselineAOTFooter) == 12, "Footer must be 12 bytes");
static_assert(sizeof(BaselineManifest) == static_cast<uint32_t>(BaselineMetadataID::Count) * 4,
              "Manifest size must match metadata count");
static_assert(sizeof(ICReturnOffsetEntry) == 8, "ICReturnOffsetEntry must be 8 bytes");
static_assert(sizeof(DispatchTablePatch) == 8, "DispatchTablePatch must be 8 bytes");

struct AOTBlobLayout {
  std::size_t codeSize;
  uint32_t prologueEndOffset;
  uint32_t interpretOpOffset;
  uint32_t dispatchTableOffset;
  uint32_t debugInstrCount;
  uint32_t debugTrapCount;
  uint32_t codeCoverageCount;
  uint32_t icReturnCount;
  uint32_t patchCount;
  uint32_t opHandlerCount;

  std::size_t prologueSize() const { return prologueEndOffset; }
  std::size_t handlersSize() const { return dispatchTableOffset - prologueEndOffset; }
  std::size_t dispatchTableSize() const { return codeSize - dispatchTableOffset; }
  std::size_t manifestSize() const { return sizeof(BaselineManifest); }
  std::size_t debugInstrSize() const { return debugInstrCount * sizeof(uint32_t); }
  std::size_t debugTrapSize() const { return debugTrapCount * sizeof(uint32_t); }
  std::size_t codeCoverageSize() const { return codeCoverageCount * sizeof(uint32_t); }
  std::size_t icReturnSize() const { return icReturnCount * sizeof(ICReturnOffsetEntry); }
  std::size_t patchSize() const { return patchCount * sizeof(DispatchTablePatch); }
  std::size_t opHandlerSize() const { return opHandlerCount * sizeof(uint32_t); }
  std::size_t footerSize() const { return sizeof(BaselineAOTFooter); }
  std::size_t manifestOffset() const { return codeSize; }
  std::size_t debugInstrOffset() const { return manifestOffset() + manifestSize(); }
  std::size_t debugTrapOffset() const { return debugInstrOffset() + debugInstrSize(); }
  std::size_t codeCoverageOffset() const { return debugTrapOffset() + debugTrapSize(); }
  std::size_t icReturnOffset() const { return codeCoverageOffset() + codeCoverageSize(); }
  std::size_t patchOffset() const { return icReturnOffset() + icReturnSize(); }
  std::size_t opHandlerOffset() const { return patchOffset() + patchSize(); }
  std::size_t footerOffset() const { return opHandlerOffset() + opHandlerSize(); }

  std::size_t metadataSize() const {
    return manifestSize() + debugInstrSize() + debugTrapSize() +
           codeCoverageSize() + icReturnSize() + patchSize() +
           opHandlerSize() + footerSize();
  }

  std::size_t blobSize() const { return codeSize + metadataSize(); }

  void dump(bool isLoad, void* blobStart = nullptr) const;
};

}  // namespace js::jit

#endif  // jit_BaselineAOT_h

