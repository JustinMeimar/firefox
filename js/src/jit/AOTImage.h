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
#  include <type_traits>

#  include "jit/AOTImageFormatGenerated.h"
#  include "js/AllocPolicy.h"
#  include "js/Vector.h"

namespace js::jit {

// [SMDOC] AOT Image
// =================
//
// An AOT image is a flat binary produced at build time and mapped when the
// process starts. It contains a header, a build identity, an artifact
// directory, serialized metadata, padding, and a page aligned code segment.
//
//   +---------------------------------------------------+
//   | AOTImageHeader     (fixed layout, HeaderSize)     |
//   +---------------------------------------------------+
//   | BuildIdentity bytes  (header.buildIdentitySize)   |
//   +---------------------------------------------------+
//   | AOTBlobDirectoryEntry[header.blobCount]           |
//   +---------------------------------------------------+
//   | Per-blob { key, fields POD, arrays } sections     |
//   +---------------------------------------------------+
//   | [padding to page alignment]                       |
//   +---------------------------------------------------+
//   | Text segment  (concatenated code, page-aligned)   |
//   +---------------------------------------------------+
//
// Each artifact contains the metadata required to reconstruct its runtime
// object. A shared schema defines the serialized layout used by readers and
// writers.
//
// The loader validates the header and build identity before exposing artifacts.
// The builder emits a finalized image from recorded artifacts.

class AOTImage;
class AOTBlobReader;
class AOTBlobWriter;

// Reads the serialized fields and arrays for one artifact in order.
class AOTBlobReader {
  friend class AOTImage;

  AOTBlobReader(const image::DirectoryEntry* entry, const uint8_t* imageBase,
                const uint8_t* textBase)
      : entry_(entry),
        code_(textBase + entry->textOffset, entry->textSize),
        key_(imageBase + entry->dataOffset, entry->keySize),
        fields_(imageBase + entry->dataOffset + entry->keySize),
        arraysCursor_(fields_ + entry->fieldsSize),
        arraysEnd_(arraysCursor_ + entry->arraysSize) {}

 public:
  mozilla::Span<const uint8_t> key() const { return key_; }
  bool arraysComplete() const { return valid_ && arraysCursor_ == arraysEnd_; }
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
    if (count > size_t(arraysEnd_ - arraysCursor_) / sizeof(T)) {
      valid_ = false;
      return {};
    }
    auto span = mozilla::Span(reinterpret_cast<const T*>(arraysCursor_), count);
    arraysCursor_ += count * sizeof(T);
    return span;
  }

 private:
  const image::DirectoryEntry* entry_;
  mozilla::Span<const uint8_t> code_;
  mozilla::Span<const uint8_t> key_;
  const uint8_t* fields_;
  const uint8_t* arraysCursor_;
  const uint8_t* arraysEnd_;
  bool valid_ = true;
};

// Collects one artifact's serialized metadata.
class AOTBlobWriter {
 public:
  mozilla::Span<const uint8_t> fields() const {
    return {fields_.begin(), fields_.length()};
  }
  mozilla::Span<const uint8_t> arrays() const {
    return {arrays_.begin(), arrays_.length()};
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
  Vector<uint8_t, 0, SystemAllocPolicy> fields_;
  Vector<uint8_t, 0, SystemAllocPolicy> arrays_;
};

// Provides a read only view of an image embedded in the binary.
class AOTImage {
 public:
  // No embedded image is available when the binary contains no linked image
  // symbols.
  static const AOTImage* embedded();

  const image::Header* header() const {
    return reinterpret_cast<const image::Header*>(base_);
  }
  mozilla::Span<const uint8_t> buildIdentity() const {
    return {base_ + header()->buildIdentityOffset, header()->buildIdentitySize};
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

 private:
  static mozilla::Maybe<AOTImage> fromBytes(mozilla::Span<const uint8_t> bytes);

  explicit AOTImage(mozilla::Span<const uint8_t> bytes) : base_(bytes.data()) {}

  const uint8_t* base_;
};

// Holds the runtime representation of a serialized inline cache stub. Encoding
// copies the relevant stub metadata. Decoding reconstructs the metadata and
// lookup key. This keeps inline cache types out of the schema generator.
struct AOTICStubMetadata {
  uint8_t cacheKind = 0;
  uint8_t makesGCCalls = 0;
  uint8_t stubDataOffset = 0;
  uint8_t localTracingSlots = 0;
  Vector<uint8_t, 0, SystemAllocPolicy> cacheIRCode;
  Vector<uint8_t, 0, SystemAllocPolicy> fieldTypes;
};

mozilla::Span<const uint8_t> CurrentAOTBuildIdentity();

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTImage_h
