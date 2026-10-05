/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTImage.h"

#  include <cstring>

#  include "jit/JitSpewer.h"

// Symbols exported by the image shim, which embeds either a recorded image or
// the empty placeholder the build generates into the objdir.
extern "C" {
extern const uint8_t aot_image_start[];
extern const uint8_t aot_image_end[];
extern const uint8_t aot_build_identity[32];
}

namespace js::jit {

mozilla::Span<const uint8_t> CurrentAOTBuildIdentity() {
  return aot_build_identity;
}

const AOTImage* AOTImage::embedded() {
  static const auto image = [] {
    size_t size = size_t(aot_image_end - aot_image_start);
    auto image = fromBytes({aot_image_start, size});
    if (!image) {
      JitSpew(JitSpew_BaselineAOT,
              "AOT image absent or invalid (size=%zu); using runtime codegen",
              size);
    }
    return image;
  }();
  return image ? &image.ref() : nullptr;
}

mozilla::Maybe<AOTImage> AOTImage::fromBytes(
    mozilla::Span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(image::Header) ||
      uintptr_t(bytes.data()) % alignof(image::Header)) {
    return mozilla::Nothing();
  }
  AOTImage img(bytes);
  const image::Header* h = img.header();
  if (h->magic != image::Magic || h->version != image::Version) {
    return mozilla::Nothing();
  }
  if (h->reserved || h->imageSize > bytes.size() ||
      h->buildIdentityOffset != sizeof(image::Header) ||
      h->buildIdentitySize != image::BuildIdentitySize ||
      uint64_t(h->buildIdentityOffset) + h->buildIdentitySize >
          h->directoryOffset ||
      h->directoryOffset % image::Alignment ||
      uint64_t(h->directoryOffset) +
              uint64_t(h->blobCount) * sizeof(image::DirectoryEntry) >
          h->textOffset ||
      h->textOffset % image::TextAlignment ||
      uint64_t(h->textOffset) + h->textSize != h->imageSize) {
    return mozilla::Nothing();
  }
  uint64_t dataStart = uint64_t(h->directoryOffset) +
                       uint64_t(h->blobCount) * sizeof(image::DirectoryEntry);
  if (h->blobCount && memcmp(img.buildIdentity().data(), aot_build_identity,
                             image::BuildIdentitySize) != 0) {
    return mozilla::Nothing();
  }
  for (uint32_t i = 0; i < h->blobCount; i++) {
    const auto& entry = img.directory()[i];
    if (entry.kind >= AOTBlobKindCount || entry.keySize % 4 ||
        entry.dataOffset % image::Alignment || entry.dataOffset < dataStart ||
        uint64_t(entry.dataOffset) + entry.keySize + entry.fieldsSize +
                entry.arraysSize >
            h->textOffset ||
        entry.textOffset % image::Alignment ||
        uint64_t(entry.textOffset) + entry.textSize > h->textSize) {
      return mozilla::Nothing();
    }
  }
  return mozilla::Some(img);
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT
