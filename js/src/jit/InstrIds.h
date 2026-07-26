/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InstrIds_h
#define jit_InstrIds_h

#include "mozilla/SHA1.h"
#include "mozilla/Span.h"

#include <cstdint>
#include <cstring>

// [SMDOC] Phase-3 instrumentation identities
//
// The instrumentation log is durable and cross-process. All identity
// values that must remain stable across processes and across runs are
// SHA-1 digests, represented as a 20-byte Sha1Digest. All identity
// values that only need to be unique within one process (dense
// numbering used to keep individual event lines short) are
// per-process monotonic uint32_t "local ids".
//
// The event schema always emits the SHA-1 identity at least once (on
// the creation/emit event for the entity). Subsequent references use
// the local id. This lets the harness parse in one pass and re-hydrate
// the local id -> SHA mapping.
//
// Domains of SHA-1 identity:
//   semantic_id  bytecode + immutable flags + fun flags + arg info +
//                IC entry count + gcthing kinds. Two scripts share
//                semantic_id iff baseline codegen would be equivalent.
//   code_id      normalized machine code bytes + relocation layout.
//   source_id    the source text of the JSScript.
//   site_id      source_id, script line, script column, bytecode PC.
//   ic_body_id   CacheIRStubInfo->code() body of the baseline stub.
//
// The hash helper deliberately does not depend on JSContext; every
// identity is a pure function of its inputs.

namespace js::jit {

struct Sha1Digest {
  uint8_t bytes[mozilla::SHA1Sum::kHashSize];

  bool operator==(const Sha1Digest& other) const {
    return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
  }
  bool operator!=(const Sha1Digest& other) const { return !(*this == other); }
};

// 40-char lowercase hex, null-terminated (41 bytes).
struct Sha1HexString {
  char chars[2 * mozilla::SHA1Sum::kHashSize + 1];
  const char* c_str() const { return chars; }
};

Sha1HexString ToHex(const Sha1Digest& d);

// One-shot SHA-1 over a byte span.
Sha1Digest Sha1(mozilla::Span<const uint8_t> data);

// Multi-part SHA-1 for chained inputs. The mixer is a thin RAII
// wrapper around mozilla::SHA1Sum whose only reason to exist is to
// give call sites a nicer name than "s.update(x, n)".
class Sha1Mixer {
  mozilla::SHA1Sum inner_;

 public:
  Sha1Mixer() = default;

  Sha1Mixer& Append(const void* data, size_t nbytes) {
    inner_.update(data, uint32_t(nbytes));
    return *this;
  }

  template <typename T>
  Sha1Mixer& AppendPod(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    inner_.update(&value, uint32_t(sizeof(T)));
    return *this;
  }

  Sha1Mixer& AppendSpan(mozilla::Span<const uint8_t> s) {
    if (!s.empty()) {
      inner_.update(s.data(), uint32_t(s.size()));
    }
    return *this;
  }

  Sha1Digest Finish() {
    Sha1Digest out;
    inner_.finish(out.bytes);
    return out;
  }
};

// Cheap 32-bit fingerprint from the first four bytes of a SHA-1 for
// hash-table keys. Full identity comparison remains 20 bytes.
inline uint32_t Prefix32(const Sha1Digest& d) {
  uint32_t v;
  std::memcpy(&v, d.bytes, sizeof(v));
  return v;
}

}  // namespace js::jit

#endif  // jit_InstrIds_h
