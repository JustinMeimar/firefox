/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TrampolineAOT_h
#define jit_TrampolineAOT_h

#include <cstdint>

namespace js::jit {

#ifdef ENABLE_AOT_TRAMPOLINES

extern "C" {
  extern const uint8_t trampoline_blob_start[];
  extern const uint8_t trampoline_blob_end[];
}

inline uint8_t* GetAOTTrampolineBlob() {
  return const_cast<uint8_t*>(trampoline_blob_start);
}

inline std::size_t GetAOTTrampolineSize() {
  return trampoline_blob_end - trampoline_blob_start;
}

/* Trampoline metadata is used at load time to reconstruct the offsets
 * stored in JitRuntime for various trampoline stubs and VM wrappers.
 */
enum class TrampolineMetadataID : uint32_t {
  // Basic trampoline offsets
  EnterJIT = 0,
  BailoutHandler,
  Invalidator,

  // Pre-barrier offsets
  ValuePreBarrier,
  StringPreBarrier,
  ObjectPreBarrier,
  ShapePreBarrier,
  WasmAnyRefPreBarrier,

  // Other stubs
  LazyLinkStub,
  InterpreterStub,
  DoubleToInt32ValueStub,

  // Exception and profiler
  ExceptionTail,
  ExceptionTailReturnValueCheck,
  ProfilerExitFrameTail,

  // Ion generic call stubs (2 entries: Call and Construct)
  IonGenericCall,
  IonGenericConstruct,

  // VM interpreter entry
  VMInterpreterEntry,

  // Size metadata
  HeaderSize,
  CodeSize,

  // Count fields for vectors
  VMWrapperCount,
  TrampolineNativeCount,

  Count
};

static const uint32_t AOT_TRAMPOLINE_MAGIC = 0x5452414D;  // 'TRAM'

struct alignas(4) TrampolineAOTFooter {
  uint32_t magic = AOT_TRAMPOLINE_MAGIC;
  uint32_t version = 1;
  uint32_t manifestOffset = 0;  // Absolute offset from blob start
};

struct alignas(4) TrampolineManifest {
  uint32_t metadata[uint32_t(TrampolineMetadataID::Count)];
  // Followed by:
  // uint32_t vmWrapperOffsets[VMWrapperCount]
  // uint32_t trampolineNativeOffsets[TrampolineNativeCount]
};

static_assert(sizeof(TrampolineAOTFooter) == 12, "Footer must be 12 bytes");
static_assert(sizeof(TrampolineManifest) ==
    static_cast<uint32_t>(TrampolineMetadataID::Count) * 4,
    "Manifest size must match metadata count");

struct TrampolineAOTLayout {
  std::size_t codeSize;
  uint32_t vmWrapperCount;
  uint32_t trampolineNativeCount;

  std::size_t manifestSize() const {
    return sizeof(TrampolineManifest);
  }

  std::size_t vmWrapperArraySize() const {
    return vmWrapperCount * sizeof(uint32_t);
  }

  std::size_t trampolineNativeArraySize() const {
    return trampolineNativeCount * sizeof(uint32_t);
  }

  std::size_t footerSize() const {
    return sizeof(TrampolineAOTFooter);
  }

  std::size_t manifestOffset() const {
    return codeSize;
  }

  std::size_t vmWrapperArrayOffset() const {
    return manifestOffset() + manifestSize();
  }

  std::size_t trampolineNativeArrayOffset() const {
    return vmWrapperArrayOffset() + vmWrapperArraySize();
  }

  std::size_t footerOffset() const {
    return trampolineNativeArrayOffset() + trampolineNativeArraySize();
  }

  std::size_t metadataSize() const {
    return manifestSize() + vmWrapperArraySize() +
           trampolineNativeArraySize() + footerSize();
  }

  std::size_t blobSize() const {
    return codeSize + metadataSize();
  }

  void dump(bool isLoad, void* blobStart = nullptr) const;
};

#endif  // ENABLE_AOT_TRAMPOLINES

}  // namespace js::jit

#endif  // jit_TrampolineAOT_h
