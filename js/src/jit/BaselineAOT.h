/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineAOT_h
#define jit_BaselineAOT_h

#include <cstdint>
#include <cstring>
#include "jit/AOTReloc.h"
#include "jit/VMFunctions.h"
#include "vm/JSContext.h"

namespace js::jit {

static constexpr const char* kAOTOutputPath =
    "js/src/jit/AOTBaselineInterpreter.S";
enum class DebugTrapHandlerKind;

#define BASELINE_MANIFEST_FIELDS(V) \
  V(InterpretOp)                    \
  V(InterpretOpNoDebugTrap)         \
  V(BailoutPrologue)                \
  V(ProfilerEnterToggle)            \
  V(ProfilerExitToggle)             \
  V(DebugTrapHandler)               \
  V(DispatchTableOffset)            \
  V(CallVMDebugPrologue)            \
  V(CallVMDebugEpilogue)            \
  V(CallVMDebugAfterYield)          \
  V(HeaderSize)                     \
  V(PrologueEndOffset)              \
  V(DebugInstrumentationCount)      \
  V(DebugTrapCount)                 \
  V(CodeCoverageCount)              \
  V(ICReturnCount)                  \
  V(RuntimePatchCount)

static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"
static constexpr uint32_t AOT_CONTAINER_VERSION = 1;

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  SelfHostedFunction = 1,
};

struct AOTContainerHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t blobCount;
  uint32_t padding;
};

static_assert(sizeof(AOTContainerHeader) == 16,
              "AOTContainerHeader must be 16 bytes");

struct AOTBlobDirectoryEntry {
  uint32_t kind;       // AOTBlobKind
  uint32_t nameHash;   // reserved for future use
  uint32_t codeOffset;
  uint32_t codeSize;
  uint32_t manifestOffset;
  uint32_t manifestSize;
  uint32_t patchesOffset;
  uint32_t patchesCount;
  uint32_t metadataOffset;
  uint32_t metadataSize;
};

static_assert(sizeof(AOTBlobDirectoryEntry) == 40,
              "AOTBlobDirectoryEntry must be 40 bytes");

extern "C" {
  extern const uint8_t bl_aot_container_start[];
  extern const uint8_t bl_aot_container_end[];
}

inline const uint8_t* GetAOTContainer() {
  return bl_aot_container_start;
}

inline size_t GetAOTContainerSize() {
  return bl_aot_container_end - bl_aot_container_start;
}

inline const AOTContainerHeader* GetAOTContainerHeader() {
  if (GetAOTContainerSize() < sizeof(AOTContainerHeader)) {
    return nullptr;
  }
  const auto* hdr = reinterpret_cast<const AOTContainerHeader*>(GetAOTContainer());
  if (hdr->magic != AOT_CONTAINER_MAGIC ||
      hdr->version != AOT_CONTAINER_VERSION) {
    return nullptr;
  }
  return hdr;
}

inline const AOTBlobDirectoryEntry* GetAOTBlobDirectory() {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return nullptr;
  return reinterpret_cast<const AOTBlobDirectoryEntry*>(
      GetAOTContainer() + sizeof(AOTContainerHeader));
}

inline const AOTBlobDirectoryEntry* FindAOTBlob(AOTBlobKind kind) {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return nullptr;
  const auto* dir = GetAOTBlobDirectory();
  for (uint32_t i = 0; i < hdr->blobCount; i++) {
    if (static_cast<AOTBlobKind>(dir[i].kind) == kind) {
      return &dir[i];
    }
  }
  return nullptr;
}

struct AOTManifestScalars {
#define DECLARE_FIELD(name) uint32_t name = 0;
  BASELINE_MANIFEST_FIELDS(DECLARE_FIELD)
#undef DECLARE_FIELD
};

// Per-script manifest for AOT-compiled self-hosted functions.
struct AOTScriptManifest {
  uint32_t warmUpCheckPrologueOffset;
  uint32_t profilerEnterToggleOffset;
  uint32_t profilerExitToggleOffset;
  uint32_t retAddrEntryCount;
  uint32_t osrEntryCount;
  uint32_t debugTrapEntryCount;
  uint32_t resumeEntryCount;
  uint32_t codeSize;
  uint32_t headerSize;
  uint32_t runtimePatchCount;
};

//TODO(Justin): Revisit why we need this hashing stuff, or can not
//reuse mozilla::Hash existing functinoality.
template <typename CharT>
inline uint32_t AOTNameHash(const CharT* chars, size_t len) {
  uint32_t h = 0;
  for (size_t i = 0; i < len; i++) {
    h = h * 31 + static_cast<uint8_t>(chars[i]);
  }
  return h;
}
inline uint32_t AOTNameHash(const char* s) {
  return AOTNameHash(s, strlen(s));
}

// Load time context required to apply patches.
struct PatchContext {
    JSContext* cx;
    uint8_t* codeBase;
    uint32_t dispatchTableOffset;
};

// While trapolines are generated at runtime we must manually patch the
// `callWithABI<Fn>` calls, since the absolute address of functions in
// the text section is non-deterministic.
enum class AOTCppFunctionId : uint32_t {
  PostWriteBarrier,
  FrameIsDebuggeeCheck,
  HandleCodeCoverageAtPrologue,
  HandleCodeCoverageAtPC,
  Count
};

enum class AOTPreBarrierIndex : uint32_t {
  Value = 0,
  String,
  Object,
  Shape,
  WasmAnyRef,
  Count
};

class RuntimePatch {
  public:
    // Each patch has a kind tag, telling us which kind of patch to apply, and a
    // targetOffset, representing at which byte we should apply the patch.
    AOTRelocKind kind;
    uint32_t targetOffset;

    // Auxiliary data — interpretation depends on kind.
    uint32_t auxData;

    static RuntimePatch DispatchTablePatch(uint32_t targetOffset_,
                                           uint32_t handlerOffset_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::DispatchTable;
      p.targetOffset = targetOffset_;
      p.auxData = handlerOffset_;
      return p;
    }

    static RuntimePatch VMWrapperPatch(uint32_t targetOffset_,
                                       VMFunctionId vmId_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::VMWrapper;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(vmId_);
      return p;
    }

    static RuntimePatch DebugTrapPatch(uint32_t targetOffset_,
                                       DebugTrapHandlerKind dbgKind_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::DebugTrapHandler;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(dbgKind_);
      return p;
    }

    static RuntimePatch CppFunctionPatch(uint32_t targetOffset_,
                                         AOTCppFunctionId fnId) {
      RuntimePatch p;
      p.kind = AOTRelocKind::CppFunction;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(fnId);
      return p;
    }

    static RuntimePatch PreBarrierPatch(uint32_t targetOffset_,
                                        AOTPreBarrierIndex idx) {
      RuntimePatch p;
      p.kind = AOTRelocKind::PreBarrier;
      p.targetOffset = targetOffset_;
      p.auxData = uint32_t(idx);
      return p;
    }

    static RuntimePatch ExceptionTailPatch(uint32_t targetOffset_) {
      RuntimePatch p;
      p.kind = AOTRelocKind::ExceptionTail;
      p.targetOffset = targetOffset_;
      p.auxData = 0;
      return p;
    }

    explicit RuntimePatch(AOTRelocKind kind_, uint32_t targetOffset_) :
      kind(kind_), targetOffset(targetOffset_) {}

    void apply(const PatchContext& pc) const;

  private:
    RuntimePatch() = default;
    uintptr_t getValueToPatch(const PatchContext& pc) const;
};

static constexpr uintptr_t AOT_PATCH_SENTINEL = 0x0000A070DEADBEEF;

static_assert(sizeof(RuntimePatch) == 12,
              "RuntimePatch size must be 12 bytes");

class BaselineInterpreter;
class JitCode;

void* ResolveCppFunction(AOTCppFunctionId id);

[[nodiscard]] JitCode* AllocateAndPatchAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    const uint8_t* containerBase, uint32_t headerSize,
    CodeKind codeKind, uint32_t dispatchTableOffset);

using RuntimePatchVector = Vector<RuntimePatch, 0, SystemAllocPolicy>;

// Build and save the interpreter AOT blob to the saved-blob slot.
// Called from BaselineInterpreterGenerator::dumpAOTInterp with all
// the data extracted from the generator.  The metadata byte vectors
// are pre-packed by the caller (debugInstr, debugTraps, coverage,
// icReturns concatenated).
[[nodiscard]] bool BuildAndSaveInterpBlob(
    JitCode* code, const AOTManifestScalars& scalars,
    const RuntimePatchVector& patches,
    const uint8_t* metadataBytes, size_t metadataSize);

// Load the AOT interpreter blob from the embedded container and
// initialize the BaselineInterpreter.
[[nodiscard]] bool LoadAOTInterpFromContainer(
    JSContext* cx, BaselineInterpreter& interpreter);

// Load a pre-compiled self-hosted function from the AOT container.
// |name| is the self-hosted function name (used for hash matching).
// Returns true if successfully loaded, false if no blob found or on error.
[[nodiscard]] bool LoadAOTSelfHosted(JSContext* cx,
                                     HandleScript script,
                                     Handle<JSAtom*> name);

// Write the final AOT .S container (interpreter blob + self-hosted blobs).
// Must be called after a realm exists. Respects dumpBaselineInterp and
// dumpBaselineSelfHosted flags to control which blobs are included.
[[nodiscard]] bool DumpAOTContainer(JSContext* cx);

}  // namespace js::jit

#endif  // jit_BaselineAOT_h
