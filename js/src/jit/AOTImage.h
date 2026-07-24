/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTImage_h
#define jit_AOTImage_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Assertions.h"
#  include "mozilla/Maybe.h"
#  include "mozilla/Span.h"

#  include <cstdint>
#  include <cstring>
#  include <ostream>
#  include <type_traits>

#  include "jstypes.h"

#  include "js/AllocPolicy.h"
#  include "js/Vector.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

// [SMDOC] AOT Image
// =================
//
// An AOT image is a flat binary produced at build time and, when
// present, mmapped at process start. Its wire format is:
//
//   +---------------------------------------------------+
//   | AOTImageHeader     (fixed layout, HeaderSize)    |
//   +---------------------------------------------------+
//   | Fingerprint bytes  (header.fingerprintSize)       |
//   +---------------------------------------------------+
//   | AOTBlobDirectoryEntry[header.blobCount]           |
//   +---------------------------------------------------+
//   | Per-blob { fields POD, arrays } sections          |
//   +---------------------------------------------------+
//   | [padding to page alignment]                       |
//   +---------------------------------------------------+
//   | Text segment  (concatenated code, page-aligned)   |
//   +---------------------------------------------------+
//
// Everything a runtime needs to reconstruct a JitCode-backed class is
// carried in the blob's fields POD and its element arrays. The typed
// layout of a blob is described in AOTImageSchema.yaml; the generator
// (GenerateAOTImage.py) emits AOTFields_<Kind> PODs, AOTView_<Kind>
// decoded views, and (when a `metadata_type:` is declared) symmetric
// Encode/Decode helpers that translate between a hand-written metadata
// class and the wire representation.
//
// Reading side: AOTImage wraps the embedded byte range, verifies magic
// and fingerprint, and exposes typed AOTBlobReaders over each entry in
// the directory.
//
// Writing side: AOTImageBuilder accumulates AOTBlobWriters and emits a
// finalized image byte stream. It is used by AOTArtifactRecorder
// (patch 10+). PackAOTImage.py (also patch 10+) shares the schema so
// tooling and runtime cannot diverge on wire layout.

class AOTImage;
class AOTBlobReader;
class AOTBlobWriter;
class AOTImageBuilder;
class JitCode;
enum class CodeKind : uint8_t;

// Blob kinds enumerated by AOTImageSchema.yaml.
// Adding a kind: extend this enum, add its yaml entry, bump image::Version.
enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  BaselineFunction = 1,
  InlineCacheStub = 2,
  Configuration = 3,
};

namespace image {

// "AOTI" in little-endian.
inline constexpr uint32_t Magic = 0x49544F41;

// Bump on any layout / schema / fingerprint-input change.
inline constexpr uint16_t Version = 3;

// SHA-1 of engine-relevant inputs (AOTSlots order, VMFunctionId order,
// ABIFUNCTION_LIST order, JSOp numbering, ISA baseline, engine version).
// Recorded at pack time; verified at load time. See patch 11's
// installer for the check.
inline constexpr uint32_t FingerprintSize = 20;

// Directory + fields padding boundary. Rows fit within a cache line.
inline constexpr uint32_t Alignment = 16;

// Text segment padding. Loader relies on this to give text its own page
// so mprotect(RX) can flip it without dragging in header bytes.
inline constexpr uint32_t TextAlignment = 4096;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t blobCount;
  uint32_t fingerprintOffset;
  uint32_t fingerprintSize;
  uint32_t directoryOffset;
  uint32_t textOffset;
  uint32_t textSize;
  uint32_t imageSize;
};

static_assert(sizeof(Header) == 36,
              "image::Header wire size; edit image::Version on change");

struct DirectoryEntry {
  uint32_t kind;
  // Cheap prefilter for identity matching. For BaselineFunction blobs
  // this is a uint32 slice of identityHash; for others it is 0.
  uint32_t probeHash;
  uint8_t identityHash[20];
  uint32_t textOffset;
  uint32_t textSize;
  uint32_t dataOffset;
  uint32_t fieldsSize;
  uint32_t arraysSize;
};

static_assert(sizeof(DirectoryEntry) == 48,
              "image::DirectoryEntry wire size; edit image::Version on change");

}  // namespace image

// Reader for the fields POD + element arrays inside one blob. Element
// arrays are read sequentially; a per-instance cursor tracks position.
class AOTBlobReader {
 public:
  AOTBlobReader(const image::DirectoryEntry* entry, const uint8_t* imageBase,
                const uint8_t* textBase)
      : entry_(entry),
        code_(textBase + entry->textOffset, entry->textSize),
        fields_(imageBase + entry->dataOffset),
        arraysCursor_(imageBase + entry->dataOffset + entry->fieldsSize),
        arraysEnd_(arraysCursor_ + entry->arraysSize) {}

  AOTBlobKind kind() const { return AOTBlobKind(entry_->kind); }
  const image::DirectoryEntry* entry() const { return entry_; }
  uint32_t fieldsSize() const { return entry_->fieldsSize; }
  mozilla::Span<const uint8_t> code() const { return code_; }
  mozilla::Span<const uint8_t> identityHash() const {
    return {entry_->identityHash, sizeof(entry_->identityHash)};
  }

  template <typename T>
  T readFields() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT fields POD must be trivially copyable");
    MOZ_ASSERT(fieldsSize() == sizeof(T));
    T out;
    memcpy(&out, fields_, sizeof(T));
    return out;
  }

  template <typename T>
  mozilla::Span<const T> readArray(uint32_t count) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT array elements must be trivially copyable");
    if (count == 0) {
      return {};
    }
    MOZ_ASSERT(arraysCursor_ + count * sizeof(T) <= arraysEnd_);
    auto span = mozilla::Span(reinterpret_cast<const T*>(arraysCursor_), count);
    arraysCursor_ += count * sizeof(T);
    return span;
  }

 private:
  const image::DirectoryEntry* entry_;
  mozilla::Span<const uint8_t> code_;
  const uint8_t* fields_;
  const uint8_t* arraysCursor_;
  const uint8_t* arraysEnd_;
};

// Accumulator for one blob's code, fields, and array sections. The
// resulting byte sequences are handed to AOTImageBuilder::addBlob.
class AOTBlobWriter {
 public:
  AOTBlobWriter(AOTBlobKind kind, uint32_t probeHash,
                const uint8_t identityHash[20])
      : kind_(kind), probeHash_(probeHash) {
    if (identityHash) {
      memcpy(identityHash_, identityHash, sizeof(identityHash_));
    } else {
      memset(identityHash_, 0, sizeof(identityHash_));
    }
  }

  AOTBlobKind kind() const { return kind_; }
  uint32_t probeHash() const { return probeHash_; }
  const uint8_t* identityHash() const { return identityHash_; }

  mozilla::Span<const uint8_t> code() const {
    return {code_.begin(), code_.length()};
  }
  mozilla::Span<const uint8_t> fields() const {
    return {fields_.begin(), fields_.length()};
  }
  mozilla::Span<const uint8_t> arrays() const {
    return {arrays_.begin(), arrays_.length()};
  }

  [[nodiscard]] bool writeCode(const uint8_t* data, size_t len) {
    return code_.append(data, len);
  }

  template <typename T>
  [[nodiscard]] bool writeFields(const T& f) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT fields POD must be trivially copyable");
    return fields_.append(reinterpret_cast<const uint8_t*>(&f), sizeof(T));
  }

  template <typename T>
  [[nodiscard]] bool writeArray(const T* data, size_t count) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT array elements must be trivially copyable");
    if (count == 0) {
      return true;
    }
    return arrays_.append(reinterpret_cast<const uint8_t*>(data),
                          count * sizeof(T));
  }

 private:
  AOTBlobKind kind_;
  uint32_t probeHash_;
  uint8_t identityHash_[20];
  Vector<uint8_t, 0, SystemAllocPolicy> code_;
  Vector<uint8_t, 0, SystemAllocPolicy> fields_;
  Vector<uint8_t, 0, SystemAllocPolicy> arrays_;
};

// Read-only view over an in-memory AOT image. The bytes may come from
// the embedded incbin symbol pair (AOTImage::embedded, patch 11+) or
// from a jsapi-test round-trip buffer.
class AOTImage {
 public:
  // Returns nullptr until patch 11 wires the embedded incbin symbols.
  static const AOTImage* embedded();

  static mozilla::Maybe<AOTImage> fromBytes(mozilla::Span<const uint8_t> bytes);

  const image::Header* header() const {
    return reinterpret_cast<const image::Header*>(base_);
  }
  mozilla::Span<const uint8_t> fingerprint() const {
    return {base_ + header()->fingerprintOffset, header()->fingerprintSize};
  }
  mozilla::Span<const uint8_t> text() const {
    return {base_ + header()->textOffset, header()->textSize};
  }
  uint32_t blobCount() const { return header()->blobCount; }

  AOTBlobReader blobAt(uint32_t index) const {
    MOZ_ASSERT(index < blobCount());
    return AOTBlobReader(directory() + index, base_,
                         base_ + header()->textOffset);
  }

  const image::DirectoryEntry* directory() const {
    return reinterpret_cast<const image::DirectoryEntry*>(
        base_ + header()->directoryOffset);
  }

  // Finds the sole blob of `kind`, or nothing.
  mozilla::Maybe<AOTBlobReader> findUnique(AOTBlobKind kind) const;

  // Finds a blob by kind + identity hash. `probeHash` is a fast reject
  // check derived from identity; pass 0 to match all.
  mozilla::Maybe<AOTBlobReader> findByIdentity(
      AOTBlobKind kind, uint32_t probeHash, const uint8_t* identityHash) const;

 private:
  explicit AOTImage(mozilla::Span<const uint8_t> bytes)
      : base_(bytes.data()), size_(bytes.size()) {}

  const uint8_t* base_;
  size_t size_;
};

// One-blob wire format used for intermediate `.aotb` files. Every
// recorder-produced file starts with an AOTBlobFileHeader; the payload
// order matches image::DirectoryEntry (fields, arrays, code). Both the
// C++ recorder (AOTRecorder.cpp) and PackAOTImage.py parse this same
// header.
struct AOTBlobFileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t kind;
  uint32_t probeHash;
  uint8_t identityHash[20];
  uint32_t fieldsSize;
  uint32_t arraysSize;
  uint32_t codeSize;
};

static_assert(sizeof(AOTBlobFileHeader) == 48,
              "AOTBlobFileHeader wire size; edit BlobFileVersion on change");

// "AOTB" in little-endian.
inline constexpr uint32_t BlobFileMagic = 0x42544F41;
inline constexpr uint16_t BlobFileVersion = 1;

// Builds an in-memory image from a sequence of AOTBlobWriters. The
// fingerprint is caller-provided so record-time versus load-time inputs
// stay symmetric.
class AOTImageBuilder {
 public:
  [[nodiscard]] bool addBlob(AOTBlobWriter&& blob) {
    return blobs_.append(std::move(blob));
  }

  uint32_t blobCount() const { return blobs_.length(); }

  // Emits the finalized image to `out`. `fingerprint` must be
  // image::FingerprintSize bytes.
  [[nodiscard]] bool finalize(std::ostream& out, const uint8_t* fingerprint);

  // As above, but collects into a byte vector. Used by the jsapi-test.
  [[nodiscard]] bool finalize(Vector<uint8_t, 0, SystemAllocPolicy>& out,
                              const uint8_t* fingerprint);

 private:
  Vector<AOTBlobWriter, 0, SystemAllocPolicy> blobs_;
};

// Allocates a JitCode over a static text range from the embedded AOT
// image. The returned JitCode has isStaticCode() == true: no
// ExecutablePool, no JitCodeHeader, no relocations. The caller must
// keep the containing AOTImage alive; embedded() images live for the
// life of the process.
[[nodiscard]] JitCode* AllocateAOTCode(JSContext* cx, uint8_t* codeStart,
                                       uint32_t codeSize, CodeKind kind);

// Wire-adjacent runtime shape of an InlineCacheStub blob. The encoder
// copies from a CacheIRStubInfo; the decoder rebuilds a
// CacheIRStubInfo (via CacheIRStubInfo::NewFromSerialized) plus its
// CacheIRStubKey::Lookup. Kept hand-written so the schema generator
// stays free of CacheIR types; only the Encode/Decode helpers are
// generated.
struct AOTICStubMetadata {
  uint8_t cacheKind = 0;
  uint8_t makesGCCalls = 0;
  uint8_t stubDataOffset = 0;
  uint8_t localTracingSlots = 0;
  Vector<uint8_t, 0, SystemAllocPolicy> cacheIRCode;
  Vector<uint8_t, 0, SystemAllocPolicy> fieldTypes;
};

struct AOTConfigurationMetadata {
  uint8_t disableInlining = 0;
  uint8_t spectreObjectMitigations = 0;
  uint8_t spectreStringMitigations = 0;
  uint8_t baselineBatching = 0;
  uint32_t baselineJitWarmUpThreshold = 0;
  uint32_t baselineQueueCapacity = 0;
  uint32_t trialInliningWarmUpThreshold = 0;

  bool operator==(const AOTConfigurationMetadata& other) const = default;
};

AOTConfigurationMetadata CurrentAOTConfiguration();

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTImage_h
