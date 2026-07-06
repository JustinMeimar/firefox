/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h


#include "mozilla/Maybe.h"
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <type_traits>
#include "jstypes.h"
#include "js/Vector.h"
#include "vm/JSContext.h"

namespace js::jit {

// [SMDOC] AOT JIT Code 

// When compiled with ENABLE_JS_AOT, SpiderMonkey can emit
// relocatable JIT code for the BaselineInterpreter, Inline
// Cache Stubs and self-hosted Baseline compiled builtins.
//
// To make JIT code relocatable, all uses of `ImmPtr` are intercepted inside
// a set of common masm interfaces then compared against a set
// of expected pointers enumerated in `AOTSlot`. If the pointer
// can be identified, the masm emits an indirection to attain the
// pointer via the &AOTIndirectionTable, stored in the BaselineFrame.
// This incurs a cost of two loads to attain any runtime pointer, but
// allows the generated code to be independent from any particular runtime.
// The buffer can then be dumped, serialized into an assembly scaffold,
// and reattached as a build input.

extern const double MathRandomScaleInv;

static constexpr uint32_t kAOTMaxVMWrappers = 512;
static constexpr uint32_t kAOTMaxABIFunctions = 256;

enum class AOTSlot : uint32_t {
#define AOT_SLOT(name, ...) name,
#define AOT_SLOT_TRAMPOLINE(name) name,
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
#undef AOT_SLOT_TRAMPOLINE
  NamedSlot_End,
  VMWrapper_Begin = NamedSlot_End,
  VMWrapper_End = VMWrapper_Begin + kAOTMaxVMWrappers,
  ABIFn_Begin = VMWrapper_End,
  ABIFn_End = ABIFn_Begin + kAOTMaxABIFunctions,
  Count = ABIFn_End
};

inline AOTSlot AOTSlotForABIFn(uint32_t idx) {
  MOZ_ASSERT(idx < kAOTMaxABIFunctions);
  return AOTSlot(uint32_t(AOTSlot::ABIFn_Begin) + idx);
}

inline AOTSlot AOTSlotForVMWrapper(uint32_t id) {
  MOZ_ASSERT(id < kAOTMaxVMWrappers);
  return AOTSlot(uint32_t(AOTSlot::VMWrapper_Begin) + id);
}

inline const char* AOTSlotName(AOTSlot slot) {
  switch (slot) {
#define AOT_SLOT(name, ...) case AOTSlot::name: return #name;
#define AOT_SLOT_TRAMPOLINE(name) case AOTSlot::name: return #name;
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
#undef AOT_SLOT_TRAMPOLINE
    default:
      break;
  }
  uint32_t s = uint32_t(slot);
  if (s >= uint32_t(AOTSlot::VMWrapper_Begin) &&
      s < uint32_t(AOTSlot::VMWrapper_End)) {
    return "VMWrapper";
  }
  return "Unknown";
}

static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"

// Bump on any layout or semantic change so stale containers are rejected
// at load time.
static constexpr uint32_t AOT_CONTAINER_VERSION = 3;

static constexpr uint32_t kAOTAlignment = 16;

// Sentinel for AOTBlobDirectoryEntry::corpusIndex meaning "not an IC-hint
// blueprint." Distinct from 0, which is a valid corpus index.
static constexpr uint32_t kNoCorpusIndex = UINT32_MAX;

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  SelfHostedFunction = 1,
  InlineCacheStub = 2,
};

// [SMDOC] AOT Container Format
//
// The on-disk (and embedded) container is a flat binary with a fixed
// header, a directory of blob entries, and the blob payloads (code,
// manifest, metadata).  Each blob carries its own kind tag
// (interpreter vs. self-hosted function, ... ) so the loader can
// lookup the code and meta-data.
struct AOTContainerHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t blobCount;
  uint32_t padding;
};

static_assert(sizeof(AOTContainerHeader) == 16,
              "AOTContainerHeader must be 16 bytes");

struct AOTBlobDirectoryEntry {
  AOTBlobKind kind;
  // Content hash of the blob's name. Populated for SelfHostedFunction so
  // getBlob() can match by function name; zero for other kinds.
  uint32_t nameHash;
  // Corpus index for InlineCacheStub, used by the eager IC-attach hint
  // machinery in initICEntries. kNoCorpusIndex for other kinds.
  uint32_t corpusIndex;
  uint32_t codeOffset;
  uint32_t codeSize;
  uint32_t metadataOffset;
  uint32_t metadataSize;
  uint32_t manifestOffset;
  uint32_t manifestSize;
};

static_assert(sizeof(AOTBlobDirectoryEntry) == 36,
              "AOTBlobDirectoryEntry must be 36 bytes");

extern "C" {
  extern const uint8_t bl_aot_container_start[];
  extern const uint8_t bl_aot_container_end[];
  extern uint8_t bl_aot_text_start[];
  extern uint8_t bl_aot_text_end[];
}

inline const uint8_t* GetAOTContainer() { return bl_aot_container_start; }
inline uint8_t* GetAOTTextBase() { return bl_aot_text_start; }
inline size_t GetAOTTextSize() { return bl_aot_text_end - bl_aot_text_start; }
inline size_t GetAOTContainerSize() { return bl_aot_container_end - bl_aot_container_start; }

inline const AOTContainerHeader* GetAOTContainerHeader() {
  if (GetAOTContainerSize() < sizeof(AOTContainerHeader)) {
    return nullptr;
  }
  const auto* hdr = reinterpret_cast<const AOTContainerHeader*>(GetAOTContainer());
  if (hdr->magic != AOT_CONTAINER_MAGIC) {
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

class JitCode;

[[nodiscard]] JitCode* AllocateAOTCode(
    JSContext* cx, const AOTBlobDirectoryEntry* entry,
    uint8_t* textBase, CodeKind codeKind);

class AOTIndirectionTable {
 public:
  AOTIndirectionTable() = default;

  void set(AOTSlot slot, uintptr_t value) {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    slots_[uint32_t(slot)] = value;
  }

  uintptr_t get(AOTSlot slot) const {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    return slots_[uint32_t(slot)];
  }

  static constexpr uint32_t offsetOfSlot(AOTSlot slot) {
    return uint32_t(slot) * sizeof(uintptr_t);
  }

  mozilla::Maybe<AOTSlot> findSlot(uintptr_t value) const;
  AOTSlot findSlotOrCrash(uintptr_t value) const;
  void dump() const;

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTSlot::Count)] = {};
};

// [SMDOC] AOT Compilation Context
//
// AOTContext is the single flag that switches the codegen pipeline into
// AOT mode.  Stack-allocated by the caller and passed to MacroAssembler
// as a non-owning pointer.  When present (non-nullptr), AOT codegen is
// active: runtime pointers are loaded via AOTIndirectionTable.
class AOTContext {
 public:
  explicit AOTContext(AOTIndirectionTable* table) : table_(table) {}
  AOTIndirectionTable* indirectionTable() const { return table_; }
 private:
  AOTIndirectionTable* table_;
};

// [SMDOC] AOT Blob Writer
//
// Accumulates code, manifest, and metadata for one AOT blob.
// Created by AOTContainerWriter::addBlob().  Movable.
class AOTBlobWriter {
  AOTBlobKind kind_;
  uint32_t nameHash_;
  uint32_t corpusIndex_;
  std::string name_;
  Vector<uint8_t, 0, SystemAllocPolicy> code_;
  Vector<uint8_t, 0, SystemAllocPolicy> manifest_;
  Vector<uint8_t, 0, SystemAllocPolicy> metadata_;

  static bool writeRawBytes(Vector<uint8_t, 0, SystemAllocPolicy>& vec,
                            const uint8_t* data, size_t len) {
    return vec.append(data, len);
  }

 public:
  AOTBlobWriter(AOTBlobKind kind, uint32_t nameHash, uint32_t corpusIndex,
                std::string name);
  AOTBlobWriter(AOTBlobWriter&&) = default;
  AOTBlobWriter& operator=(AOTBlobWriter&&) = default;

  AOTBlobKind kind() const { return kind_; }
  uint32_t nameHash() const { return nameHash_; }
  uint32_t corpusIndex() const { return corpusIndex_; }
  const std::string& name() const { return name_; }
  mozilla::Span<const uint8_t> codeBytes() const {
    return {code_.begin(), code_.length()};
  }
  mozilla::Span<const uint8_t> manifestBytes() const {
    return {manifest_.begin(), manifest_.length()};
  }
  mozilla::Span<const uint8_t> metadataBytes() const {
    return {metadata_.begin(), metadata_.length()};
  }

  bool writeCode(const uint8_t* data, size_t len) {
    return writeRawBytes(code_, data, len);
  }

  size_t codeSize() const { return code_.length(); }

  template <typename T>
  bool writeManifest(const T& m) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT manifests must be trivially copyable");
    return writeRawBytes(manifest_,
                         reinterpret_cast<const uint8_t*>(&m), sizeof(T));
  }

  template <typename T>
  bool writeMetadataArray(mozilla::Span<T> data) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT metadata elements must be trivially copyable");
    if (data.empty()) return true;
    return writeRawBytes(metadata_,
                         reinterpret_cast<const uint8_t*>(data.data()),
                         data.size() * sizeof(T));
  }

  bool writeRawMetadata(const uint8_t* data, size_t len) {
    return writeRawBytes(metadata_, data, len);
  }

  template <typename... Ts>
  bool writeMetadata(mozilla::Span<Ts>... spans) {
    return (writeMetadataArray(spans) && ...);
  }
};

// [SMDOC] AOT Container Writer
//
// Builds the multi-blob AOT container and emits the assembly .S file.
// Blobs are added via addBlob() which returns a movable AOTBlobWriter.
// Call finalize() after all blobs are populated.
class AOTContainerWriter {
  Vector<AOTBlobWriter, 0, SystemAllocPolicy> blobs_;

 public:
  [[nodiscard]] bool addBlob(AOTBlobWriter&& blob,
                             AOTBlobWriter** out = nullptr) {
    if (!blobs_.append(std::move(blob))) return false;
    if (out) *out = &blobs_.back();
    return true;
  }
  uint32_t blobCount() const { return blobs_.length(); }
  bool finalize(std::ostream& out);
};

// [SMDOC] AOT Blob Reader
//
// Typed view into one blob within the embedded AOT container.
// Metadata arrays are read sequentially via readMetadataArray<T>(count),
// which advances an internal cursor.  These methods are non-const because
// reading mutates the cursor state; callers should not hold const refs
// if they intend to read metadata.
class AOTBlobReader {
  const AOTBlobDirectoryEntry* entry_;
  const uint8_t* codeBase_;
  const uint8_t* manifestBase_;
  [[maybe_unused]] const uint8_t* metadataBase_;  // NOTE(aot): DEBUG bounds-assert only
  const uint8_t* metadataCursor_;

  friend class AOTContainerReader;

  AOTBlobReader(const AOTBlobDirectoryEntry* entry,
                const uint8_t* codeBase,
                const uint8_t* manifestBase,
                const uint8_t* metadataBase)
    : entry_(entry),
      codeBase_(codeBase),
      manifestBase_(manifestBase),
      metadataBase_(metadataBase),
      metadataCursor_(metadataBase) {}

 public:
  const AOTBlobDirectoryEntry* entry() const { return entry_; }

  mozilla::Span<const uint8_t> code() const {
    return {codeBase_, entry_->codeSize};
  }
  uint32_t codeSize() const { return entry_->codeSize; }

  template <typename T>
  T readManifest() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT manifests must be trivially copyable");
    MOZ_ASSERT(entry_->manifestSize == sizeof(T));
    T m;
    memcpy(&m, manifestBase_, sizeof(T));
    return m;
  }

  template <typename T>
  mozilla::Span<const T> readMetadataArray(uint32_t count) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT metadata elements must be trivially copyable");
    if (count == 0) {
      return {};
    }
#ifdef DEBUG
    const uint8_t* metadataEnd = metadataBase_ + entry_->metadataSize;
    MOZ_ASSERT(metadataCursor_ + count * sizeof(T) <= metadataEnd);
#endif
    auto span = mozilla::Span(
        reinterpret_cast<const T*>(metadataCursor_), count);
    metadataCursor_ += count * sizeof(T);
    return span;
  }
};

// [SMDOC] AOT Container Reader
//
// Wraps the embedded AOT container.  Provides lookup by blob kind
// and optional name hash, returning typed AOTBlobReader instances.
class AOTContainerReader {
  const AOTBlobDirectoryEntry* dir_;
  const uint8_t* containerBase_;
  const uint8_t* textBase_;
  uint32_t blobCount_;

  AOTContainerReader(const AOTBlobDirectoryEntry* dir,
                     const uint8_t* containerBase,
                     const uint8_t* textBase,
                     uint32_t blobCount)
    : dir_(dir),
      containerBase_(containerBase),
      textBase_(textBase),
      blobCount_(blobCount) {}

 public:
  static mozilla::Maybe<AOTContainerReader> fromEmbedded();

  mozilla::Maybe<AOTBlobReader> getBlob(AOTBlobKind kind,
                                        uint32_t nameHash = 0) const;

  template <typename Fn>
  bool forEachBlob(AOTBlobKind kind, Fn&& fn) const {
    bool found = false;
    for (uint32_t i = 0; i < blobCount_; i++) {
      if (dir_[i].kind != kind) continue;
      if (dir_[i].codeSize == 0 && dir_[i].manifestSize == 0 &&
          dir_[i].metadataSize == 0) {
        continue;
      }
      AOTBlobReader reader(
          &dir_[i],
          textBase_ + dir_[i].codeOffset,
          containerBase_ + dir_[i].manifestOffset,
          containerBase_ + dir_[i].metadataOffset);
      fn(reader);
      found = true;
    }
    return found;
  }
};

}  // namespace js::jit

#endif  // jit_AOT_h
