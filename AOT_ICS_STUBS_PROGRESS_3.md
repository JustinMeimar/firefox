# AOT IC Stubs — Session 3 Progress

## What Was Done

### 1. callWithABI → Indirection Table Slots (Complete, Compiles)

Replaced the `dladdr`-based `.quad <symbol>` approach for `callWithABI` targets
with indirection table slots, eliminating the local-linkage visibility problem
entirely.

**The problem:** `dladdr()` can't resolve ~59 of ~60 ABI functions because
`-fvisibility=hidden` makes them LOCAL HIDDEN in the ELF symbol table. Even
functions that DO resolve need the linker to handle `.quad <symbol>` across
object files, which fails for static members and template instantiations.

**The fix:** Route all `callWithABI<Sig, fun>()` calls through the AOT
indirection table in AOT mode. Each ABI function gets a slot, populated at
runtime from `ABIFUNCTION_LIST`. No visibility changes, no linker involvement.

**Files changed:**
- `AOT.h` — Added `ABIFn_Begin..ABIFn_End` region (256 slots) to `AOTSlot`
  enum, plus `AOTSlotForABIFn()` helper. `kAOTMaxABIFunctions = 256`.
- `Ion.cpp` — In `populateAOTIndirectionTable()`, iterate `ABIFUNCTION_LIST`
  and `ABIFUNCTION_AND_TYPE_LIST` to fill ABIFn slots with function pointers.
  Uses `JS_FUNC_TO_DATA_PTR` and `static_cast` for overloaded functions.
- `MacroAssembler-inl.h` — In `callWithABI<Sig, fun>()`, AOT branch does
  `findSlotOrCrash(addr)` → `emitAOTSlotLoad(slot, r10)` →
  `callWithABINoProfiler(r10, result)`.

### 2. Warp/Ion Static Code Guard (Complete, Compiles)

**The problem:** When Ion/Warp tries to tier-up a function using AOT IC stubs,
`WarpOracle::maybeInlineIC` calls `stub->jitCode()` →
`JitCode::FromExecutable(stubCode_)`. This does reverse pointer arithmetic
(`buffer - sizeof(JitCodeHeader)`) which only works for JIT-pool code. For
static `.text` code from the AOT container, there's no `JitCodeHeader` —
the assert `!code->isStaticCode_` fires → SIGSEGV.

This was the dominant crash cause (~1/3 of all tests). Every test that ran
long enough for Ion to attempt inlining an AOT IC stub crashed.

**The fix:** In `WarpOracle::maybeInlineIC`, check if the stub's code pointer
falls within the AOT `.text` range (`bl_aot_text_start..bl_aot_text_end`).
If so, return `Ok()` without adding a snapshot — the standard bail-out
pattern. Ion still compiles the function, just doesn't inline that IC site.

**Files changed:**
- `WarpOracle.cpp` — Added `#include "jit/AOT.h"`, guard after
  `stub = firstStub->toCacheIRStub()` checking
  `stub->stubCodeRaw() >= GetAOTTextBase()`.
- `BaselineIC.h` — Added `uint8_t* stubCodeRaw() const` public accessor
  on `ICStub`.

### 3. Stub Frame `emitAOTSlotLoad` Fix (Complete, Compiles)

**The problem:** `emitAOTSlotLoad` loads the indirection table base from
`[FramePointer + BaselineFrame::reverseOffsetOfAOTTableBase()]`. Inside a
stub frame (entered by `AutoStubFrame::enter` for VM calls, native calls,
exit frames), `FramePointer` (`rbp`) points at the stub frame, NOT the
BaselineFrame. So `[rbp - 0x20]` reads a pushed JS value instead of
`aotTableBase_`.

This affects ALL `emitAOTSlotLoad` usage inside stub frames:
`loadJSContext`, `movePtr(RelocImmPtr)`, `callWithABI`, barriers, etc.

**The fix:** Added `inAOTStubFrame_` flag to `MacroAssembler`. When set,
`emitAOTSlotLoad` chases `[FramePointer]` one extra level to get the saved
BaselineFrame pointer before accessing `aotTableBase_`:
```
Normal:     [rbp + aotTableBase_offset] → table, [table + slot] → value  (2 loads)
Stub frame: [rbp] → old_rbp, [old_rbp + aotTableBase_offset] → table, [table + slot] → value  (3 loads)
```

**Files changed:**
- `MacroAssembler.h` — Added `bool inAOTStubFrame_` field,
  `enterAOTStubFrame()`/`leaveAOTStubFrame()` methods.
- `AOTMacroAssembler.cpp` — `emitAOTSlotLoad` checks `inAOTStubFrame_`
  and dereferences `[FramePointer]` first if set.
- `BaselineCacheIRCompiler.cpp` — `AutoStubFrame::enter/leave` toggles
  the flag on the masm when `isAOT()`.

## Current State

After all three fixes, bootstrapping, and testing:
- ~8% test failure rate (down from ~60% at session start, ~33% after fix 1)
- Most remaining failures are in `asm.js/`, `auto-regress/`, `arguments/`,
  `arrays/`, `arrow-functions/` categories
- All failures are SIGSEGV (exit code -11), likely from remaining stale
  pointer issues in IC stubs or other `emitAOTSlotLoad` context issues

## Known Remaining Issues

### Unguarded `ImmPtr(&DeadObjectProxy::singleton)`
`BaselineCacheIRCompiler.cpp:382` — `branchPtr` against
`ImmPtr(&DeadObjectProxy::singleton)`. There IS a `DeadObjectProxySingleton`
slot in the indirection table but this callsite doesn't use it. Affects
cross-compartment wrapper tests.

### Potential stub frame edge cases
The `[rbp]` chase in stub frames assumes the saved `rbp` at `[FramePointer]`
is always the BaselineFrame. If there are nested stub frames or other frame
layouts where this doesn't hold, the chase would fail. Needs investigation
of specific crash sites.

### `dladdr` path still exists but is mostly dead
`emitAsmBytesWithRelocs` in `BaselineAOT.cpp` still has the `dladdr` fallback
for any remaining extended jump table entries. With the slot-based
`callWithABI`, these should no longer appear for ABI function targets. The
`dladdr` path handles any other reloc types (if any exist).

### Container version
`AOT_CONTAINER_VERSION` is still 3. Should bump since the indirection table
layout changed (256 new ABIFn slots after VMWrapper_End).

### Existing 4 CppFn slots overlap with ABIFn slots
`CppFn_PostWriteBarrier`, `CppFn_FrameIsDebuggeeCheck`, etc. are in both
`AOT_CORE_SLOTS` and `ABIFUNCTION_LIST`. Two slots point to the same function.
Harmless but should be cleaned up when factoring CppFn into a side table.

## Files Modified (This Session)

1. `js/src/jit/AOT.h` — ABIFn slot region in enum
2. `js/src/jit/Ion.cpp` — ABIFn slot population from ABIFUNCTION_LIST
3. `js/src/jit/MacroAssembler-inl.h` — callWithABI AOT slot intercept
4. `js/src/jit/MacroAssembler.h` — `inAOTStubFrame_` flag + accessors
5. `js/src/jit/AOTMacroAssembler.cpp` — stub-frame-aware emitAOTSlotLoad
6. `js/src/jit/BaselineCacheIRCompiler.cpp` — AutoStubFrame enter/leave hooks
7. `js/src/jit/WarpOracle.cpp` — static code guard for maybeInlineIC
8. `js/src/jit/BaselineIC.h` — `stubCodeRaw()` accessor

## Recommended Next Steps

1. **GDB the remaining crashes** — Get backtraces on a few failing tests
   (e.g. `Set/symbols.js`, `asm.js/testBasic.js`, `arguments/args-exists.js`)
   to determine if they share a root cause or are distinct issues.

2. **Fix `ImmPtr(&DeadObjectProxy::singleton)` — route through
   `emitAOTSlotLoad(AOTSlot::DeadObjectProxySingleton, ...)`.

3. **Audit stub frame assumptions** — verify the `[rbp]` → BaselineFrame
   chase is correct in all contexts where IC stubs enter stub frames.

4. **Consider disabling AOT ICs for asm.js** — asm.js has different frame
   layouts and calling conventions. AOT IC stubs may not be compatible.
