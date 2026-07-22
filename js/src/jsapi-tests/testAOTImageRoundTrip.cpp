/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTImage.h"
#  include "jit/AOTImageGenerated.h"
#  include "jit/BaselineJIT.h"

#  include "jsapi-tests/tests.h"

using namespace js;
using namespace js::jit;

static BaselineInterpreterMetadata SampleMetadata() {
  BaselineInterpreterMetadata md;
  md.interpretOpOffset = 0x10;
  md.interpretOpNoDebugTrapOffset = 0x24;
  md.bailoutPrologueOffset = 0x40;
  md.profilerEnterToggleOffset = 0x50;
  md.profilerExitToggleOffset = 0x60;
  md.debugTrapHandlerOffset = 0x70;
  md.callVMOffsets.debugPrologueOffset = 0x80;
  md.callVMOffsets.debugEpilogueOffset = 0x90;
  md.callVMOffsets.debugAfterYieldOffset = 0xa0;

  MOZ_RELEASE_ASSERT(md.debugInstrumentationOffsets.append(uint32_t(1)));
  MOZ_RELEASE_ASSERT(md.debugInstrumentationOffsets.append(uint32_t(2)));

  MOZ_RELEASE_ASSERT(md.debugTrapOffsets.append(uint32_t(3)));

  MOZ_RELEASE_ASSERT(md.codeCoverageOffsets.append(uint32_t(4)));
  MOZ_RELEASE_ASSERT(md.codeCoverageOffsets.append(uint32_t(5)));
  MOZ_RELEASE_ASSERT(md.codeCoverageOffsets.append(uint32_t(6)));

  MOZ_RELEASE_ASSERT(md.icReturnOffsets.append(
      BaselineInterpreterMetadata::ICReturnOffset(uint32_t(7), JSOp::Nop)));
  return md;
}

BEGIN_TEST(testAOTImageRoundTrip_BaselineInterpreter) {
  BaselineInterpreterMetadata src = SampleMetadata();

  AOTBlobWriter blob(AOTBlobKind::BaselineInterpreter, /* probeHash = */ 0,
                     /* identityHash = */ nullptr);
  CHECK(EncodeBlob_BaselineInterpreter(blob, src));

  uint8_t code[] = {0x90, 0xc3};
  CHECK(blob.writeCode(code, sizeof(code)));

  AOTImageBuilder builder;
  CHECK(builder.addBlob(std::move(blob)));

  uint8_t fingerprint[js::jit::image::FingerprintSize] = {};
  for (uint8_t& b : fingerprint) b = 0xab;

  Vector<uint8_t, 0, SystemAllocPolicy> bytes;
  CHECK(builder.finalize(bytes, fingerprint));

  mozilla::Maybe<AOTImage> img =
      AOTImage::fromBytes({bytes.begin(), bytes.length()});
  CHECK(img.isSome());
  CHECK_EQUAL(img->blobCount(), 1u);

  mozilla::Maybe<AOTBlobReader> reader =
      img->findUnique(AOTBlobKind::BaselineInterpreter);
  CHECK(reader.isSome());
  CHECK_EQUAL(reader->code().size(), sizeof(code));
  CHECK(memcmp(reader->code().data(), code, sizeof(code)) == 0);

  BaselineInterpreterMetadata dst;
  CHECK(DecodeBlob_BaselineInterpreter(reader.ref(), &dst));

  CHECK_EQUAL(dst.interpretOpOffset, src.interpretOpOffset);
  CHECK_EQUAL(dst.interpretOpNoDebugTrapOffset,
              src.interpretOpNoDebugTrapOffset);
  CHECK_EQUAL(dst.bailoutPrologueOffset, src.bailoutPrologueOffset);
  CHECK_EQUAL(dst.profilerEnterToggleOffset, src.profilerEnterToggleOffset);
  CHECK_EQUAL(dst.profilerExitToggleOffset, src.profilerExitToggleOffset);
  CHECK_EQUAL(dst.debugTrapHandlerOffset, src.debugTrapHandlerOffset);
  CHECK_EQUAL(dst.callVMOffsets.debugPrologueOffset,
              src.callVMOffsets.debugPrologueOffset);
  CHECK_EQUAL(dst.callVMOffsets.debugEpilogueOffset,
              src.callVMOffsets.debugEpilogueOffset);
  CHECK_EQUAL(dst.callVMOffsets.debugAfterYieldOffset,
              src.callVMOffsets.debugAfterYieldOffset);

  CHECK_EQUAL(dst.debugInstrumentationOffsets.length(),
              src.debugInstrumentationOffsets.length());
  for (size_t i = 0; i < src.debugInstrumentationOffsets.length(); i++) {
    CHECK_EQUAL(dst.debugInstrumentationOffsets[i],
                src.debugInstrumentationOffsets[i]);
  }
  CHECK_EQUAL(dst.debugTrapOffsets.length(), src.debugTrapOffsets.length());
  for (size_t i = 0; i < src.debugTrapOffsets.length(); i++) {
    CHECK_EQUAL(dst.debugTrapOffsets[i], src.debugTrapOffsets[i]);
  }
  CHECK_EQUAL(dst.codeCoverageOffsets.length(),
              src.codeCoverageOffsets.length());
  for (size_t i = 0; i < src.codeCoverageOffsets.length(); i++) {
    CHECK_EQUAL(dst.codeCoverageOffsets[i], src.codeCoverageOffsets[i]);
  }
  CHECK_EQUAL(dst.icReturnOffsets.length(), src.icReturnOffsets.length());
  for (size_t i = 0; i < src.icReturnOffsets.length(); i++) {
    CHECK_EQUAL(dst.icReturnOffsets[i].offset, src.icReturnOffsets[i].offset);
    CHECK(dst.icReturnOffsets[i].op == src.icReturnOffsets[i].op);
  }

  CHECK(memcmp(img->fingerprint().data(), fingerprint,
               js::jit::image::FingerprintSize) == 0);

  return true;
}
END_TEST(testAOTImageRoundTrip_BaselineInterpreter)

#endif  // ENABLE_JS_AOT
