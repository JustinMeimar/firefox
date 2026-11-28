/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MonoBaseline_h
#define jit_MonoBaseline_h

#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"

#include <stddef.h>
#include <stdint.h>

#include "jit/CacheIRCompiler.h"
#include "jit/CompactBuffer.h"
#include "jit/MonoStats.h"
#include "util/Memory.h"

class JSScript;
class JSTracer;
struct JSContext;
struct JSRuntime;

namespace js {

class LifoAlloc;

namespace jit {

class BaselineFrame;
class ICCacheIRStub;
class ICScript;
class Label;
class MacroAssembler;
class MonoStubTemplate;
class TempAllocator;
struct TrampolinePtr;
enum class CacheKind : uint8_t;

// [SMDOC] Monomorphic Baseline
//
// A script whose IC ops each have at most one stub when Baseline compiles it,
// or with the mixed option, a script with any IC op that has exactly one, is
// compiled monomorphic. IC ops with more stubs or none call their IC chain.
// Each IC op with one stub that makes no calls has the stub emitted inline,
// with its stub fields as constants and its failure paths out of line next to
// its exit. Each other IC op with one stub calls a copy of that stub instead
// of the first stub of its IC chain, so it runs the same shared stub code. The
// copy is owned by the BaselineScript, which traces its edges strongly, and
// its next stub is an exit for the op. A stub that calls a scripted function
// is not copied, since trial inlining may replace it in the IC chain with one
// passing the callee an ICScript of its own, and reads its entered count.
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
// The compile takes the ICs of the script on the main thread when it starts:
// the info of the only stub of each monomorphic IC, and a copy of the stub. It
// reads nothing else of the ICs, so it can run off thread like other Baseline
// compiles, where inline stubs can only be copied from templates (see
// MonoTemplates.h).
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
//
// On x64, inline stubs exit through a shared trampoline instead, as Ion code
// bails out: each state of the inputs that failures leave gets a call to the
// trampoline out of line, and discarding the stubs makes their nops calls to
// it too. The trampoline stores the registers and finds the exit by the
// return address of the call in the exit table of the script, which encodes
// for each inline stub its pc, the values to push and, for each state, where
// the inputs are. Code then only has a call per state:
//
//     nop5                         ; becomes a call to the trampoline
//     cmp   tag(R0), Object
//     jne   state0
//     unbox R0 -> R0
//     cmp   shape(R0), $shape
//     jne   state1
//     ...
//
//   Out of line, after the epilogue:
//
//   state0:
//     call  trampoline             ; the input in R0
//   state1:
//     call  trampoline             ; the input in R0, unboxed as an object

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
  // Copies the stub, whose edges must be strong. Returns nullptr on OOM.
  static MonoStub* New(ICCacheIRStub* stub, TrampolinePtr exit,
                       uint32_t pcOffset);
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
// of an entry to its exit that drops a return address, or zero if it exits
// through the trampoline. Discarding the stubs of the script makes the nop a
// call to the exit, so that the stub exits when it runs next.
struct MonoInlineStub {
  uint32_t stubOffset;
  uint32_t exitOffset;
};

// Where a value an exit pushes comes from.
enum class MonoExitValue : uint8_t {
  // An input of the stub, from where it is in the state of the exit.
  Input,
  Local,
  Arg,
  This,
  Register,
  Constant,
};

// The most values an exit pushes.
static constexpr size_t MonoExitMaxValues = 8;

// The exits of the inline stubs of monomorphic code, with the constants they
// push. See the SMDOC above.
class MonoExitTable {
  uint32_t numConstants_;
  uint32_t length_;

  MonoExitTable(uint32_t numConstants, uint32_t length)
      : numConstants_(numConstants), length_(length) {}

  Value* constants() { return reinterpret_cast<Value*>(this + 1); }
  const uint8_t* data() const {
    return reinterpret_cast<const uint8_t*>(this + 1) +
           numConstants_ * sizeof(Value);
  }

 public:
  static MonoExitTable* New(mozilla::Span<const uint64_t> constants,
                            const CompactBufferWriter& sites);

  const Value& constant(uint32_t index) {
    MOZ_ASSERT(index < numConstants_);
    return constants()[index];
  }
  const uint8_t* dataStart() const { return data(); }
  const uint8_t* dataEnd() const { return data() + length_; }

  void trace(JSTracer* trc);
};

// A jump of an inline stub to the call to the trampoline for one of its exit
// states.
struct MonoExitJump {
  uint32_t offset;
  uint32_t state;
};

using MonoExitJumpVector = Vector<MonoExitJump, 0, SystemAllocPolicy>;

// Encodes the exit table of monomorphic code, an inline stub at a time: its
// site, then the values its exits push, then its states. The states of a stub
// don't depend on its site, so they are encoded once with its code, and its
// exit only copies them.
class MonoExitTableWriter {
  CompactBufferWriter sites_;
  Vector<uint64_t, 0, SystemAllocPolicy> constants_;
  uint32_t lastStubOffset_ = 0;
  uint32_t lastCallsOffset_ = 0;

 public:
  // Encodes the states that the failures of an inline stub leave its inputs
  // in, the first being the original one, in which discarded stubs exit too,
  // and appends the jumps of its failures to each state, by state. Returns
  // false on OOM.
  [[nodiscard]] static bool encodeStates(
      MacroAssembler& masm, mozilla::Span<const OperandLocation> origInputs,
      mozilla::Span<FailurePath> failures, CompactBufferWriter& states,
      MonoExitJumpVector& jumps);

  void writeSite(uint32_t stubOffset, uint32_t pcOffset, uint32_t drop,
                 uint32_t numValues);
  void writeValue(MonoExitValue kind, uint32_t index);
  [[nodiscard]] bool writeConstant(const Value& value);

  // Writes the encoded states of the site, whose states with jumps each have
  // a call to the trampoline, one after another from the given offset.
  void writeStates(mozilla::Maybe<uint32_t> callsOffset,
                   mozilla::Span<const uint8_t> states);

  bool oom() const { return sites_.oom(); }
  MonoExitTable* finish();
};

// An IC as monomorphic code is compiled from it. The ICs of a script are taken
// on the main thread when its compile starts, so that the code can be
// compiled on any thread.
struct MonoIC {
  // The stub info of the only stub of the IC if it is monomorphic, else null.
  const CacheIRStubInfo* info;
  // A copy of that stub if it can be copied, else null. Its edges are strong.
  ICCacheIRStub* stub;
  // Whether the IC has no stub.
  bool empty;
};

// Why the script is or isn't compiled monomorphic.
MonoScriptReason MonoReasonFor(JSContext* cx, JSScript* script,
                               bool compileDebugInstrumentation);

// Whether monomorphic code can be compiled off thread, where its inline stubs
// can only be copied from templates.
bool CanCompileMonoOffThread();

// Takes the ICs of the script, copying their stubs into alloc. Returns
// nullptr on OOM.
MonoIC* TakeMonoICs(JSContext* cx, JSScript* script, LifoAlloc& alloc);
void TraceMonoICs(JSTracer* trc, MonoIC* ics, size_t numICs);

// Why the stub of the IC can't be emitted inline, with the op that can't be
// in *op.
MonoNoInline WhyNotInlineMonoStub(const MonoIC& ic, CacheOp* op);

// Emits the stub inline, with its inputs known to have the given types, if
// any, appending its original input locations and its failure paths for
// emitting out of line. Returns false on OOM.
[[nodiscard]] bool EmitInlinedMonoStub(
    JSContext* cx, MacroAssembler& masm, TempAllocator& alloc,
    ICCacheIRStub* stub, mozilla::Span<const JSValueType> inputTypes,
    InlinedInputVector& origInputs, InlinedFailureVector& failures);

// Emits the stub inline for code with an exit table, copied from the template
// if one is given, else compiled, appending its encoded exit states and its
// jumps to them. Returns false on OOM.
[[nodiscard]] bool EmitInlinedMonoStubWithExitTable(
    JSContext* cx, MacroAssembler& masm, TempAllocator& alloc, JS::Zone* zone,
    ICCacheIRStub* stub, const MonoStubTemplate* templ,
    mozilla::Span<const JSValueType> inputTypes, CompactBufferWriter& states,
    MonoExitJumpVector& jumps);

// The known types of the stubs of the info, computed once.
const CacheIRKnownTypes& MonoStubKnownTypes(const CacheIRStubInfo* info);

// Puts back the operands of an inline stub and continues at the op in the
// Baseline Interpreter.
void EmitMonoExit(MacroAssembler& masm, MonoOperands operands,
                  uint32_t pcOffset, TrampolinePtr exitTail);

// Whether the IC has exactly one stub and has never failed to attach another.
bool IsMonomorphicIC(ICScript* icScript, size_t entryIndex);

// Whether every IC of the script that has a stub is monomorphic, and one has.
bool IsMonomorphicScript(JSScript* script);

// Whether any IC of the script is monomorphic.
bool HasMonomorphicIC(JSScript* script);

// A hash of the first stub of each IC of the script and of whether it is the
// only one.
mozilla::HashNumber HashMonoICs(JSScript* script);

// Whether the script runs monomorphic code, or would if Baseline compiled it
// now. Monomorphic code profiles only into the ICScript of its script.
bool RunsMonoCode(JSScript* script);

// Called by the exits with the pc offset of the op. Returns the address at
// which the Baseline Interpreter continues.
uint8_t* MonoBaselineExit(BaselineFrame* frame, uint32_t pcOffset);

// Called by the trampoline that the exits of code with an exit table call,
// with the general purpose registers, indexed by their code, stored below the
// return address of the call. Pushes the values of the exit, stores the stack
// pointer from which to continue in place of the stored stack pointer, and
// returns the address at which the Baseline Interpreter continues.
uint8_t* MonoBailout(BaselineFrame* frame, uintptr_t* regs);

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
