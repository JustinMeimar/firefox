/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MonoBaseline_h
#define jit_MonoBaseline_h

#include "mozilla/Maybe.h"

#include <stdint.h>

#include "jit/CacheIR.h"
#include "util/Memory.h"

class JSScript;
class JSTracer;
struct JSContext;

namespace js {
namespace jit {

class BaselineFrame;
class CacheIRStubInfo;
class CacheIRWriter;
class ICCacheIRStub;
class Label;
class MacroAssembler;
class TempAllocator;
struct TrampolinePtr;

// [SMDOC] Monomorphic Baseline
//
// A script whose IC ops each have at most one stub when Baseline compiles it
// is compiled monomorphic: each IC op with a stub emits that stub's CacheIR
// inline instead of calling its IC chain. The stub data is copied into the
// BaselineScript, which traces its edges strongly.
//
// Baseline keeps the stack synced at IC ops, and stubs fail only before they
// have effects, so a failing guard only has to put back the operands the op
// took off the stack to have the frame the Baseline Interpreter expects at the
// op. It then continues there, in the same frame.
//
// The first such exit marks the script polymorphic. The script stops entering
// its monomorphic code, and once it warms up again in the Baseline Interpreter
// and no frame runs that code anymore, it is compiled with regular IC calls.

// The stub data of a stub inlined in Baseline code.
class InlinedStub {
  InlinedStub* next_ = nullptr;
  CacheIRStubInfo* stubInfo_;

  explicit InlinedStub(CacheIRStubInfo* stubInfo) : stubInfo_(stubInfo) {}

 public:
  static InlinedStub* New(const CacheIRWriter& writer, CacheKind kind);
  static void Destroy(InlinedStub* stub);
  static void DestroyList(InlinedStub* list);

  static constexpr uint32_t stubDataOffset() {
    return AlignBytes(sizeof(InlinedStub), sizeof(uint64_t));
  }

  InlinedStub* next() const { return next_; }
  void setNext(InlinedStub* next) { next_ = next; }

  void trace(JSTracer* trc);
};

void TraceInlinedStubs(JSTracer* trc, InlinedStub* list);

// Whether every IC op of the script that has a stub has exactly one and has
// never failed to attach another.
bool IsMonomorphicScript(JSScript* script);

// Where the operands an op took off the stack before its IC are when its
// inlined stub fails.
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
};

mozilla::Maybe<MonoOperands> MonoOperandsFor(CacheKind kind, uint32_t popped);

// Emits the stub inline, with failures jumping to failure. Sets *inlined to
// the stub data the code uses, or to nullptr if the stub can't be inlined.
[[nodiscard]] bool EmitInlinedStub(JSContext* cx, MacroAssembler& masm,
                                   TempAllocator& alloc, ICCacheIRStub* stub,
                                   CacheKind kind, Label* failure,
                                   InlinedStub** inlined);

// Puts the operands back on the stack and continues at the op in the Baseline
// Interpreter.
void EmitMonoExit(MacroAssembler& masm, MonoOperands operands,
                  uint32_t pcOffset, TrampolinePtr exitStub);

// Called by the exit stub with the pc offset of the op. Returns the address
// at which the Baseline Interpreter continues.
uint8_t* MonoBaselineExit(BaselineFrame* frame, uint32_t pcOffset);

// Whether the script's BaselineScript is monomorphic code that failed.
bool HasFailedMonoBaseline(JSScript* script);

// Discards the failed monomorphic code of the script so that it can be
// compiled with IC calls, unless a frame still runs it. Returns whether it was
// discarded.
bool MaybeDiscardFailedMonoBaseline(JSContext* cx, JSScript* script);

}  // namespace jit
}  // namespace js

#endif /* jit_MonoBaseline_h */
