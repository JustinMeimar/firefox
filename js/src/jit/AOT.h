/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h


#include "mozilla/HashTable.h"
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
//
// When built with `ENABLE_JS_AOT`, SpiderMonkey emits relocatable JIT
// code for the baseline interpreter, inline cache stubs, and self-hosted
// builtins.
//
// An AOT container is a flat binary blob, embedded into the shell via
// AOTBaselineStub.S and mmapped at runtime. Its payload is a sequence of
// AOT artifacts (Blobs). A single IC, baseline interpreter, or
// self-hosted function is one blob. Each blob is packed as
// { code, fields POD, element arrays }; the container writes a directory
// entry describing the code offset/size and the shared data offset with
// per-blob fields and arrays sizes. From there, AOT artifacts can be
// reconstructed and deserialized into their respective C++ classes, only
// this time with a JitCode backed by static memory rather than dynamic
// heap memory.
//
// To make JIT code relocatable, all uses of `ImmPtr` are intercepted
// inside a set of common masm interfaces then compared against a set of
// expected pointers enumerated in `AOTSlot`. On a match, the masm emits
// an indirection to load the pointer through the `AOTIndirectionTable`
// owned by JitRuntime.
//
// Table access, per call path:
//   Baseline frame:      frame slot -> table base -> slot value
//                        (BaselineFrame::reverseOffsetOfAOTTableBase())
//   Baseline stub frame: frame slot -> table base -> slot value
//                        (BaselineStubFrameLayout::AOTTableOffsetFromFP)
// Both are two loads. The frame slot is seeded at prologue from
// AOTTablePassReg, which the AOT preamble (JitRuntime::generateAOTPreamble)
// stashes with &aotIndirectionTable before jumping to the entry. Pinning
// AOTTablePassReg through emit (to reach one load) is future work; today
// the register is only live from preamble to prologue.

extern const double MathRandomScaleInv;
class JitCode;

static constexpr uint32_t kAOTMaxVMWrappers = 512;
static constexpr uint32_t kAOTMaxABIFunctions = 256;
static constexpr uint32_t kNoCorpusIndex = UINT32_MAX; // Used for IC hints
static constexpr uint32_t kAOTAlignment = 16;
static constexpr uint32_t AOT_CONTAINER_VERSION = 11;
static constexpr uint32_t AOT_CONTAINER_MAGIC = 0x414F5443;  // "AOTC"

// Bump AOT_CONTAINER_VERSION on any change to the wire format:
// AOTContainerHeader, AOTBlobDirectoryEntry, the fields POD of any blob
// (see AOTBlobSchema.yaml), AOTSlot enum renumbering, or fingerprint
// contents. A stale container that matches the old magic but not the new
// layout otherwise deserializes into garbage.
//
// v11: BaselineFunction blobs' `nameHash` is the O(1) probe key
// (SharedImmutableScriptData::hash(); see ComputeBaselineProbeHash in
// BaselineAOT), not a hash of the canonical bytes. The canonical-bytes
// memcmp inside the blob remains the ground-truth verify.

// The container fingerprint POD (AOTCodegenOptions) is schema-defined
// in jit/AOTBlobSchema.yaml and emitted into jit/AOTBlobGenerated.h.


enum class AOTSlot : uint32_t {
#define AOT_SLOT(name, ...) name,
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
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
#include "jit/AOTSlots.tbl"
#undef AOT_SLOT
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

enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  InlineCacheStub = 1,
  // Per-script AOT baseline code. Applies to both self-hosted builtins
  // and guest scripts; identity is canonical byte content (see
  // ComputeBaselineCanonical), so origin is irrelevant.
  BaselineFunction = 2,
};

// The container is a flat binary. Layout:
//   AOTContainerHeader          (16 bytes)
//   fingerprint bytes           (header.fingerprintSize bytes)
//   [padding to kAOTAlignment]
//   AOTBlobDirectoryEntry[]     (header.blobCount entries)
//   [padding]
//   { fields, arrays }*         (per blob, at dataOffset in directory)
//
// Fingerprint lives in the header region and is verified inside
// AOTContainerReader::fromEmbedded before any blob is exposed.
struct AOTContainerHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t blobCount;
  uint32_t fingerprintSize;
};

static_assert(sizeof(AOTContainerHeader) == 16,
              "AOTContainerHeader must be 16 bytes");

// Directory entry for one blob. `dataOffset` points at the fields POD;
// arrays begin at `dataOffset + fieldsSize` (they are always adjacent).
struct AOTBlobDirectoryEntry {
  AOTBlobKind kind;
  uint32_t nameHash;
  uint32_t corpusIndex; // used only in IC hint prototype
  uint32_t codeOffset;
  uint32_t codeSize;
  uint32_t dataOffset;
  uint32_t fieldsSize;
  uint32_t arraysSize;
};

static_assert(sizeof(AOTBlobDirectoryEntry) == 32,
              "AOTBlobDirectoryEntry must be 32 bytes");

// These symbols are defined by AOTBaselineStub.S, regenerated with
// `--aot-dump-baseline --aot-dump-self-hosted`. Pre-bootstrap the stub is
// empty and the runtime consumers must handle a zero-sized container.
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

// Offset of the blob directory from the container base. Header +
// fingerprint bytes + alignment padding.
inline size_t AOTBlobDirectoryOffset(const AOTContainerHeader* hdr) {
  size_t raw = sizeof(AOTContainerHeader) + hdr->fingerprintSize;
  return (raw + (kAOTAlignment - 1)) & ~size_t(kAOTAlignment - 1);
}

inline const AOTBlobDirectoryEntry* GetAOTBlobDirectory() {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) return nullptr;
  return reinterpret_cast<const AOTBlobDirectoryEntry*>(
      GetAOTContainer() + AOTBlobDirectoryOffset(hdr));
}

// Pointer to fingerprint bytes in the header region, and their length.
inline const uint8_t* GetAOTContainerFingerprint(size_t* outLen) {
  const auto* hdr = GetAOTContainerHeader();
  if (!hdr) {
    *outLen = 0;
    return nullptr;
  }
  *outLen = hdr->fingerprintSize;
  return GetAOTContainer() + sizeof(AOTContainerHeader);
}

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

// Switches codegen to emit position-independent code that routes runtime
// pointers through the indirection table. Its presence on the
// MacroAssembler is the mode switch: `isAOT()` returns true whenever an
// AOTContext is attached. Stack-allocate at the top of a codegen scope
// and pass to MacroAssembler; prefer AutoAOTCodegen (AutoAOTCodegen.h),
// which handles the RAII attach/detach.
class AOTContext {
 public:
  explicit AOTContext(AOTIndirectionTable* table) : table_(table) {}
  AOTIndirectionTable* indirectionTable() const { return table_; }
 private:
  AOTIndirectionTable* table_;
};

// Accumulates code, fields POD, and element arrays for one AOT blob.
// Created by AOTContainerWriter::addBlob().  Movable.
class AOTBlobWriter {
  AOTBlobKind kind_;
  uint32_t nameHash_;
  uint32_t corpusIndex_;
  std::string name_;
  Vector<uint8_t, 0, SystemAllocPolicy> code_;
  Vector<uint8_t, 0, SystemAllocPolicy> fields_;
  Vector<uint8_t, 0, SystemAllocPolicy> arrays_;

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
  void setNameHash(uint32_t h) { nameHash_ = h; }
  uint32_t corpusIndex() const { return corpusIndex_; }
  const std::string& name() const { return name_; }
  mozilla::Span<const uint8_t> codeBytes() const {
    return {code_.begin(), code_.length()};
  }
  mozilla::Span<const uint8_t> fieldsBytes() const {
    return {fields_.begin(), fields_.length()};
  }
  mozilla::Span<const uint8_t> arraysBytes() const {
    return {arrays_.begin(), arrays_.length()};
  }

  bool writeCode(const uint8_t* data, size_t len) {
    return writeRawBytes(code_, data, len);
  }

  size_t codeSize() const { return code_.length(); }

  template <typename T>
  bool writeFields(const T& f) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT fields POD must be trivially copyable");
    return writeRawBytes(fields_,
                         reinterpret_cast<const uint8_t*>(&f), sizeof(T));
  }

  template <typename T>
  bool writeArray(mozilla::Span<T> data) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT array elements must be trivially copyable");
    if (data.empty()) return true;
    return writeRawBytes(arrays_,
                         reinterpret_cast<const uint8_t*>(data.data()),
                         data.size() * sizeof(T));
  }
};

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


// Typed view into one blob within the embedded AOT container.
// Element arrays are read sequentially via readArray<T>(count),
// which advances an internal cursor.  These methods are non-const because
// reading mutates the cursor state, callers should not hold const refs
// if they intend to read arrays.
class AOTBlobReader {
  const AOTBlobDirectoryEntry* entry_;
  const uint8_t* codeBase_;
  const uint8_t* fieldsBase_;
#ifdef DEBUG
  const uint8_t* arraysBase_;
#endif
  const uint8_t* arraysCursor_;

  friend class AOTContainerReader;

  AOTBlobReader(const AOTBlobDirectoryEntry* entry,
                const uint8_t* codeBase,
                const uint8_t* fieldsBase,
                const uint8_t* arraysBase)
    : entry_(entry),
      codeBase_(codeBase),
      fieldsBase_(fieldsBase),
#ifdef DEBUG
      arraysBase_(arraysBase),
#endif
      arraysCursor_(arraysBase) {}

 public:
  const AOTBlobDirectoryEntry* entry() const { return entry_; }

  mozilla::Span<const uint8_t> code() const {
    return {codeBase_, entry_->codeSize};
  }

  template <typename T>
  T readFields() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT fields POD must be trivially copyable");
    MOZ_ASSERT(entry_->fieldsSize == sizeof(T));
    T f;
    memcpy(&f, fieldsBase_, sizeof(T));
    return f;
  }

  template <typename T>
  mozilla::Span<const T> readArray(uint32_t count) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT array elements must be trivially copyable");
    if (count == 0) {
      return {};
    }
#ifdef DEBUG
    const uint8_t* arraysEnd = arraysBase_ + entry_->arraysSize;
    MOZ_ASSERT(arraysCursor_ + count * sizeof(T) <= arraysEnd);
#endif
    auto span = mozilla::Span(
        reinterpret_cast<const T*>(arraysCursor_), count);
    arraysCursor_ += count * sizeof(T);
    return span;
  }
};

// Wraps the embedded AOT container.  Provides lookup by blob kind
// and optional name hash, returning typed AOTBlobReader instances.
class AOTContainerReader {
 public:
  using ProbeSet =
      mozilla::HashSet<uint32_t, mozilla::DefaultHasher<uint32_t>,
                       SystemAllocPolicy>;

 private:
  const AOTBlobDirectoryEntry* dir_;
  const uint8_t* containerBase_;
  const uint8_t* textBase_;
  uint32_t blobCount_;
  // Populated once at fromEmbedded() time from every BaselineFunction
  // blob's nameHash (v11: SharedImmutableScriptData::hash()). Used as
  // the O(1) miss check in LoadAOTBaselineFunction so scripts absent
  // from the corpus never pay ComputeBaselineCanonical.
  const ProbeSet* baselineProbes_;

  AOTContainerReader(const AOTBlobDirectoryEntry* dir,
                     const uint8_t* containerBase,
                     const uint8_t* textBase,
                     uint32_t blobCount,
                     const ProbeSet* baselineProbes)
    : dir_(dir),
      containerBase_(containerBase),
      textBase_(textBase),
      blobCount_(blobCount),
      baselineProbes_(baselineProbes) {}

  AOTBlobReader makeReader(uint32_t i) const {
    const uint8_t* fieldsBase = containerBase_ + dir_[i].dataOffset;
    return AOTBlobReader(
        &dir_[i],
        textBase_ + dir_[i].codeOffset,
        fieldsBase,
        fieldsBase + dir_[i].fieldsSize);
  }

  static bool isEmpty(const AOTBlobDirectoryEntry& e) {
    return e.codeSize == 0 && e.fieldsSize == 0 && e.arraysSize == 0;
  }

  // Two-tier traversal: this private helper is a bool-returning
  // any_of over blobs of the given kind (`fn` returns true to short-
  // circuit, and the return value is true iff `fn` short-circuited);
  // the public `forEachBlob`/`forEachBlobWithHash` are void-returning
  // wrappers that swallow the bool for simple traversal.
  template <typename Fn>
  bool anyBlob(AOTBlobKind kind, Fn&& fn) const {
    for (uint32_t i = 0; i < blobCount_; i++) {
      if (dir_[i].kind != kind) continue;
      if (isEmpty(dir_[i])) continue;
      AOTBlobReader reader = makeReader(i);
      if (fn(reader)) return true;
    }
    return false;
  }

 public:
  static mozilla::Maybe<AOTContainerReader> fromEmbedded();

  mozilla::Maybe<AOTBlobReader> getBlob(AOTBlobKind kind,
                                        uint32_t nameHash = 0) const;

  template <typename Fn>
  void forEachBlob(AOTBlobKind kind, Fn&& fn) const {
    anyBlob(kind, [&](AOTBlobReader& reader) {
      fn(reader);
      return false;
    });
  }

  // Iterate every blob matching (kind, nameHash). Correctness never
  // depends on the hash being unique: callers must verify content
  // (memcmp / fields comparison) before installing. fn returning true
  // short-circuits iteration.
  template <typename Fn>
  void forEachBlobWithHash(AOTBlobKind kind, uint32_t nameHash,
                           Fn&& fn) const {
    anyBlob(kind, [&](AOTBlobReader& reader) {
      if (reader.entry()->nameHash != nameHash) return false;
      return bool(fn(reader));
    });
  }

  bool hasBaselineProbe(uint32_t probeHash) const {
    return baselineProbes_ && baselineProbes_->has(probeHash);
  }
};

}  // namespace js::jit

#endif  // jit_AOT_h
