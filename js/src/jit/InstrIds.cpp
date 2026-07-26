/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/InstrIds.h"

namespace js::jit {

Sha1Digest Sha1(mozilla::Span<const uint8_t> data) {
  Sha1Mixer m;
  m.AppendSpan(data);
  return m.Finish();
}

Sha1HexString ToHex(const Sha1Digest& d) {
  static constexpr char kHex[] = "0123456789abcdef";
  Sha1HexString out;
  for (size_t i = 0; i < sizeof(d.bytes); ++i) {
    out.chars[2 * i] = kHex[(d.bytes[i] >> 4) & 0xf];
    out.chars[2 * i + 1] = kHex[d.bytes[i] & 0xf];
  }
  out.chars[2 * sizeof(d.bytes)] = '\0';
  return out;
}

}  // namespace js::jit
