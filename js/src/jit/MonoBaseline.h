/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MonoBaseline_h
#define jit_MonoBaseline_h

#include "mozilla/Maybe.h"
#include "mozilla/Span.h"

#include <stddef.h>
#include <stdint.h>

#include "jit/CacheIRCompiler.h"
#include "jit/MonoStats.h"
#include "util/Memory.h"

class JSScript;
class JSTracer;
struct JSContext;
struct JSRuntime;

namespace js {
namespace jit {

class BaselineFrame;
class ICCacheIRStub;
class Label;
class MacroAssembler;
class TempAllocator;
struct TrampolinePtr;
enum class CacheKind : uint8_t;

// [SMDOC] Monomorphic Baseline
//
// A script whose IC ops each have at most one stub when Baseline compiles it
// is compiled monomorphic. Each IC op with a stub that makes no calls has the
// stub emitted inline, with its stub fields as constants and its failure paths
// out of line next to its exit. Each other IC op with a stub calls a copy of
// that stub instead of the first stub of its IC chain, so it runs the same
// shared stub code. The copy is owned by the BaselineScript, which traces its
// edges strongly, and its next stub is an exit for the op.
//
// Stubs fail only before they have effects, so when a stub fails the exit
// only has to rebuild the frame the Baseline Interpreter expects at the op: it
// pushes the stack values the compiler had not stored yet, then puts back the
// operands the op took off the stack. Calls need every stack value stored, so
// only inline stubs leave values unstored. The exit then continues at the op
// in the Baseline Interpreter, in the same frame.
//
// The first such exit marks the script polymorphic. The script stops entering
// its monomorphic code, and once it warms up again in the Baseline Interpreter
// and no frame runs that code anymore, it is compiled with IC chains.
//
// When the IC stubs of a script whose monomorphic code is on the stack are
// discarded, its stubs are discarded too: copies fail right away, inline stubs
// exit when they run next, and the script stops entering the code without
// being marked polymorphic. Frames can't be stopped inside an inline stub,
// since inline stubs make no calls that can run script or collect garbage.
//
// For example, an inline GetProp stub that loads a fixed slot, on x64:
//
//     nop5                         ; becomes a call to inval on discard
//     cmp   tag(R0), Object
//     jne   exit                   ; nothing to restore yet
//     unbox R0 -> R0               ; the input is unboxed in place
//     cmp   shape(R0), $shape
//     jne   restore                ; the input must be boxed again
//     load  slot(R0) -> R0
//     ...                          ; next op
//
//   Out of line, after the epilogue:
//
//   inval:
//     add   rsp, 8                 ; drop the return address of the call
//   exit:
//     push  unstored stack values
//     mov   $pcOffset, ICStubReg
//     jmp   exit tail              ; continues in the Baseline Interpreter
//   restore:
//     box   Object, R0 -> R0
//     jmp   exit

// Where the operands an op took off the stack before its IC are when its stub
// fails.
enum class MonoOperands : uint8_t {
  // Still on the stack.
  InPlace,
  // The top operand in R0.
  R0,
  // The top two operands in R0 and R1.
  R0R1,
  // The object in R0 and the value in R1, with the value on the stack where
  // the object was.
  SetProp,
  // The object in R0 and the key in R1, with the value on the stack where the
  // object was.
  SetElem,

  Count
};

mozilla::Maybe<MonoOperands> MonoOperandsFor(CacheKind kind, uint32_t popped);

// The most inputs a stub takes from the stack.
static constexpr size_t MonoMaxStackInputs = 3;

// The type through which the edges of copied stubs are traced.
class MonoStubCopy;

// A stub copied for monomorphic code, with the exit its failures take and a
// link to the next copied stub of the script. The copy follows it.
class MonoStub {
 public:
  // The exit is an ICStub as far as stub code is concerned: failures load its
  // code and jump there with the stub register pointing to it.
  struct Exit {
    uint8_t* code;
    uint32_t pcOffset;
    // Set when the stub was discarded with the script's IC stubs, after which
    // the copy fails without running.
    bool discarded;
  };

 private:
  MonoStub* next_ = nullptr;
  Exit exit_;

  explicit MonoStub(const Exit& exit) : exit_(exit) {}

  static constexpr size_t offsetOfStub() {
    return AlignBytes(sizeof(MonoStub), sizeof(uint64_t));
  }

 public:
  // Copies the stub, or returns nullptr with *oom false if it can't be
  // copied.
  static MonoStub* New(JSContext* cx, ICCacheIRStub* stub, uint32_t pcOffset,
                       MonoOperands operands, bool* oom);
  static void DestroyList(MonoStub* list);

  static constexpr size_t offsetOfExitPcOffset() {
    return offsetof(Exit, pcOffset);
  }

  ICCacheIRStub* stub() {
    return reinterpret_cast<ICCacheIRStub*>(reinterpret_cast<uint8_t*>(this) +
                                            offsetOfStub());
  }

  void setNext(MonoStub* next) { next_ = next; }

  static void TraceList(JSTracer* trc, MonoStub* list);
  static void DiscardList(JSRuntime* rt, MonoStub* list);
};

// An inline stub, by the code offsets of the end of the nop it starts with and
// of an entry to its exit that drops a return address. Discarding the stubs of
// the script makes the nop a call to the exit, so that the stub exits when it
// runs next.
struct MonoInlineStub {
  uint32_t stubOffset;
  uint32_t exitOffset;
};

// Whether the stub can be emitted inline.
bool CanInlineMonoStub(ICCacheIRStub* stub);

// Why the stub can't be emitted inline, with the op that can't be in *op.
MonoNoInline WhyNotInlineMonoStub(ICCacheIRStub* stub, CacheOp* op);

// Emits the stub inline, with its inputs known to have the given types, if
// any, appending its original input locations and its failure paths for
// emitting out of line. Returns false on OOM, with *inlined false if the stub
// could not be emitted.
[[nodiscard]] bool EmitInlinedMonoStub(
    JSContext* cx, MacroAssembler& masm, TempAllocator& alloc,
    ICCacheIRStub* stub, mozilla::Span<const JSValueType> inputTypes,
    InlinedInputVector& origInputs, InlinedFailureVector& failures,
    bool* inlined);

// Narrows the types of the first inputs of the stub to the types its guards
// check, which they have once it ran without failing.
void NarrowMonoStubInputTypes(const CacheIRStubInfo* info,
                              mozilla::Span<JSValueType> types);

// Puts back the operands of an inline stub and continues at the op in the
// Baseline Interpreter.
void EmitMonoExit(MacroAssembler& masm, MonoOperands operands,
                  uint32_t pcOffset, TrampolinePtr exitTail);

// Whether every IC op of the script that has a stub has exactly one and has
// never failed to attach another.
bool IsMonomorphicScript(JSScript* script);

// Called by the exits with the pc offset of the op. Returns the address at
// which the Baseline Interpreter continues.
uint8_t* MonoBaselineExit(BaselineFrame* frame, uint32_t pcOffset);

// Makes the monomorphic code of a script whose IC stubs are discarded exit
// without running its stubs, and stops entering it.
void DiscardMonoStubs(JSRuntime* rt, JSScript* script);

// Discards the failed monomorphic code of the script so that it can be
// compiled with IC chains, unless a frame still runs it. Returns whether it
// was discarded.
bool MaybeDiscardFailedMonoBaseline(JSContext* cx, JSScript* script);

}  // namespace jit
}  // namespace js

#endif /* jit_MonoBaseline_h */
