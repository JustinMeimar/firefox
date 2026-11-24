/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MonoTemplates_h
#define jit_MonoTemplates_h

#include "mozilla/HashFunctions.h"
#include "mozilla/Span.h"

#include <stddef.h>
#include <stdint.h>

#include "jit/CacheIRCompiler.h"
#include "jit/MonoStats.h"
#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"

struct JSContext;

namespace js {
namespace jit {

class ICCacheIRStub;
class MacroAssembler;
class MonoStubTemplateBuilder;

// [SMDOC] Monomorphic Baseline stub templates
//
// The inline stubs of monomorphic Baseline code (see MonoBaseline.h) are
// copied from templates, so that a stub whose code was seen before costs a
// copy and a few patches instead of a CacheIR compile. A template is the
// inline code of a stub with holes for its stub fields. Its key is the
// CacheIR of the stub, the known types of its inputs, and the fields that
// can't be holes. Templates hold no GC things and nothing specific to a realm
// or zone outside of holes, so the JitRuntime keeps them for its lifetime.
//
// A field can be a hole when every op reading it embeds it only as an
// immediate or displacement, on x64 an int32 as 4 bytes and a pointer or Value
// as 8. Values whose encoding depends on their bits, like doubles, are part of
// the key instead.
//
// Each stub info remembers the templates last used for its stubs, so that
// most lookups compare the ends of keys instead of hashing the CacheIR.
//
// A template is built by compiling the stub inline three times with its hole
// fields replaced by different sentinels. The sentinels are wide enough that
// the assembler can't pick a shorter encoding for them, so writing any value
// in their place gives the code compiling with that value would, without its
// peepholes. GC thing sentinels point into the arena of the thing they
// replace, so that assertions about cells still hold. The holes are where the
// three compiles have the sentinels of a field. The compiles must differ
// nowhere else, and have the same relocations, jumps and failure paths.
// Their code may point into no zone or realm, except to the flag of the zone
// that pre-barriers test, which is a hole too. Otherwise the template is
// rejected, and stubs with its key are compiled directly.
//
// An instance of a template is its code appended to the Baseline code, with
// the fields of the stub written into the holes, data relocations for its GC
// things, its calls, and the jumps to its failure paths threaded onto fresh
// labels for its exit.
//
// Templates are only built on x64.

class MonoStubTemplate {
 public:
  enum class HoleKind : uint8_t {
    Int32Field,
    WordField,
    GCThingField,
    // A GC thing field boxed as a Value of the hole's type.
    BoxedGCThingField,
    ZoneNeedsMarkingBarrier,
  };

  struct Hole {
    uint32_t offset;
    uint32_t fieldOffset;
    HoleKind kind;
    JSValueType boxedType;
  };

  struct PendingJump {
    uint32_t offset;
    void* target;
    RelocationKind kind;
  };

 private:
  Vector<uint8_t, 0, SystemAllocPolicy> key_;
  HashNumber hash_;

  // The end of the key, in which stubs of the same stub info differ.
  uint32_t keySuffixLength_ = 0;

  // Set when no template can be built for the key.
  bool rejected_ = false;

  Vector<uint8_t, 0, SystemAllocPolicy> code_;
  Vector<Hole, 0, SystemAllocPolicy> holes_;
  Vector<PendingJump, 0, SystemAllocPolicy> jumps_;
  Vector<OperandLocation, 0, SystemAllocPolicy> origInputs_;
  Vector<FailurePath, 0, SystemAllocPolicy> failures_;

  // The offsets of the jumps to the failure paths, those of the i-th path
  // ending at failureJumpsEnd_[i].
  Vector<uint32_t, 0, SystemAllocPolicy> failureJumps_;
  Vector<uint32_t, 0, SystemAllocPolicy> failureJumpsEnd_;

  friend class MonoStubTemplateBuilder;
  friend class MonoStubTemplates;

 public:
  explicit MonoStubTemplate(HashNumber hash) : hash_(hash) {}

  mozilla::Span<const uint8_t> key() const { return key_; }
  mozilla::Span<const uint8_t> keySuffix() const {
    return mozilla::Span<const uint8_t>(key_).Last(keySuffixLength_);
  }
  HashNumber hash() const { return hash_; }
  bool rejected() const { return rejected_; }
  size_t codeLength() const { return code_.length(); }

  // Appends the code for the stub with the given stub data, its original
  // input locations and its failure paths. Returns false on OOM.
  [[nodiscard]] bool instantiate(MacroAssembler& masm, JS::Zone* zone,
                                 const uint8_t* stubData,
                                 InlinedInputVector& origInputs,
                                 InlinedFailureVector& failures) const;
};

class MonoStubTemplates {
  struct Hasher {
    struct Lookup {
      mozilla::Span<const uint8_t> key;
      HashNumber hash;
    };
    static HashNumber hash(const Lookup& lookup) { return lookup.hash; }
    static bool match(const UniquePtr<MonoStubTemplate>& entry,
                      const Lookup& lookup);
  };

  HashSet<UniquePtr<MonoStubTemplate>, Hasher, SystemAllocPolicy> templates_;

 public:
  // Emits the stub inline from the template for its key, building the
  // template if there is none. Sets *use to how the stub was emitted, leaving
  // the stub to be compiled directly when that's None or Rejected. Returns
  // false on OOM.
  [[nodiscard]] bool emitStub(JSContext* cx, MacroAssembler& masm,
                              ICCacheIRStub* stub,
                              mozilla::Span<const JSValueType> inputTypes,
                              InlinedInputVector& origInputs,
                              InlinedFailureVector& failures,
                              MonoTemplateUse* use);
};

}  // namespace jit
}  // namespace js

#endif /* jit_MonoTemplates_h */
