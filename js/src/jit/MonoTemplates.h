/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MonoTemplates_h
#define jit_MonoTemplates_h

#include "mozilla/HashFunctions.h"
#include "mozilla/Span.h"

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "jit/CacheIRCompiler.h"
#include "jit/MonoBaseline.h"
#include "jit/MonoStats.h"
#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "threading/Mutex.h"
#include "util/TrailingArray.h"
#include "vm/MutexIDs.h"

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
// nowhere else, and have the same relocations, jumps and exit states.
// Their code may point into no zone or realm, except to the flag of the zone
// that pre-barriers test, which is a hole too. Otherwise the template is
// rejected, and stubs with its key are compiled directly.
//
// An instance of a template is its code appended to the Baseline code, with
// the fields of the stub written into the holes, data relocations for its GC
// things, its calls, and its exit states, encoded for the exit table when the
// template is built, with the jumps to them.
//
// Templates are only built on x64, for code with an exit table. Compiles off
// thread don't build them: a stub with no template yet is called as a copy
// instead, and its template is built when the code is linked.

// A template is a single allocation, holding these arrays after the header:
//
//    PendingJump[]   jumps()
//    Hole[]          holes()
//    MonoExitJump[]  exitJumps()
//    uint8_t[]       key()
//    uint8_t[]       code()
//    uint8_t[]       exitStates()
class alignas(uintptr_t) MonoStubTemplate final
    : public TrailingArray<MonoStubTemplate> {
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

  // What a template is made of, which it copies.
  struct Parts {
    mozilla::Span<const uint8_t> key;
    // The end of the key, in which stubs of the same stub info differ.
    uint32_t keySuffixLength;
    HashNumber hash;
    // Set when no template can be built for the key, which leaves the rest
    // empty.
    bool rejected;
    mozilla::Span<const PendingJump> jumps;
    mozilla::Span<const Hole> holes;
    mozilla::Span<const MonoExitJump> exitJumps;
    mozilla::Span<const uint8_t> code;
    mozilla::Span<const uint8_t> exitStates;
  };

 private:
  HashNumber hash_;
  uint32_t keySuffixLength_;
  bool rejected_;

  Offset holesOffset_ = 0;
  Offset exitJumpsOffset_ = 0;
  Offset keyOffset_ = 0;
  Offset codeOffset_ = 0;
  Offset exitStatesOffset_ = 0;
  Offset endOffset_ = 0;

  MonoStubTemplate(HashNumber hash, uint32_t keySuffixLength, bool rejected)
      : hash_(hash), keySuffixLength_(keySuffixLength), rejected_(rejected) {}

  static constexpr Offset jumpsOffset() { return sizeof(MonoStubTemplate); }

  template <typename T>
  Offset initElements(Offset start, mozilla::Span<const T> elements) {
    MOZ_ASSERT(isAlignedOffset<T>(start));
    std::uninitialized_copy(elements.begin(), elements.end(),
                            offsetToPointer<T>(start));
    return start + elements.size() * sizeof(T);
  }

  template <typename T>
  mozilla::Span<const T> makeSpan(Offset start, Offset end) const {
    return mozilla::Span{offsetToPointer<T>(start), numElements<T>(start, end)};
  }

  mozilla::Span<const PendingJump> jumps() const {
    return makeSpan<PendingJump>(jumpsOffset(), holesOffset_);
  }
  mozilla::Span<const Hole> holes() const {
    return makeSpan<Hole>(holesOffset_, exitJumpsOffset_);
  }
  mozilla::Span<const MonoExitJump> exitJumps() const {
    return makeSpan<MonoExitJump>(exitJumpsOffset_, keyOffset_);
  }
  mozilla::Span<const uint8_t> code() const {
    return makeSpan<uint8_t>(codeOffset_, exitStatesOffset_);
  }
  mozilla::Span<const uint8_t> exitStates() const {
    return makeSpan<uint8_t>(exitStatesOffset_, endOffset_);
  }

 public:
  // Returns nullptr on OOM.
  static MonoStubTemplate* New(const Parts& parts);

  mozilla::Span<const uint8_t> key() const {
    return makeSpan<uint8_t>(keyOffset_, codeOffset_);
  }
  mozilla::Span<const uint8_t> keySuffix() const {
    return key().Last(keySuffixLength_);
  }
  HashNumber hash() const { return hash_; }
  bool rejected() const { return rejected_; }

  // Appends the code for the stub with the given stub data, its encoded exit
  // states and its jumps to them. Returns false on OOM.
  [[nodiscard]] bool instantiate(MacroAssembler& masm, JS::Zone* zone,
                                 const uint8_t* stubData,
                                 CompactBufferWriter& states,
                                 MonoExitJumpVector& stateJumps) const;
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

  // Guards the templates and the recent templates of stub infos, since
  // compiles off thread look them up.
  Mutex lock_{mutexid::MonoStubTemplates};

 public:
  // Looks up the template for the key of the stub, building it if there is
  // none and cx is given. Sets *templ to it if the stub can be copied from it,
  // else to nullptr, and *use to why. Returns false on OOM.
  [[nodiscard]] bool lookup(JSContext* cx, ICCacheIRStub* stub,
                            mozilla::Span<const JSValueType> inputTypes,
                            const MonoStubTemplate** templ,
                            MonoTemplateUse* use);
};

}  // namespace jit
}  // namespace js

#endif /* jit_MonoTemplates_h */
