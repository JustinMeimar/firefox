# AOT IC Stubs — Session 6 Progress

## What Was Done

### 1. `callWithABI` Timing Fix (Complete, Compiles, Tested)

**The problem:** The AOT `callWithABI` intercept in `MacroAssembler-inl.h` loaded
the function pointer into `r10` via `emitAOTSlotLoad` BEFORE the move resolver
ran (`callWithABIPre`). If any ABI argument source register happened to be `r10`,
the function pointer load clobbered it. The move resolver then read the wrong
value, passing garbage to the callee.

This manifested as hash assertion failures in Map/Set IC stubs
(`AssertSetObjectHash` / `AssertMapObjectHash`), where the high register pressure
in `orderedHashTableLookup` made `r10` conflicts likely. 12 tests failed:
`cacheir/map-get-string`, `cacheir/set-has-value`, `warp/map-*`, `warp/set-*`,
`collections/*`, `debug/*`.

**The fix:** Restructured both `callWithABI` AOT intercepts (DynFn and template
forms) to call `callWithABIPre` first (resolves all pending argument moves), THEN
`emitAOTSlotLoad` into `r10`, THEN `call(r10)`, THEN `callWithABIPost`. This
matches the pattern already used by `callWithABINoProfiler(Register)` in
`x64/MacroAssembler-x64.cpp:1010`.

**Files changed:**
- `MacroAssembler-inl.h` — Both `callWithABI` AOT intercepts reordered

**Impact:** Eliminated all 12 hash assertion test failures.

### 2. Store JitCode* on ICCacheIRStub (Complete, Compiles, Tested)

**The problem:** `ICStub::jitCode()` calls `JitCode::FromExecutable(stubCode_)`,
which does reverse pointer arithmetic assuming a `JitCodeHeader` precedes the code
buffer. For AOT static stubs (code in `.text`), there is no header — the assert
`!code->isStaticCode_` fires and crashes.

This forced a `return Ok()` guard in `WarpOracle::maybeInlineIC` that skipped
transpilation entirely for AOT stubs. Without transpilation, Ion had no type info
for those IC sites, preventing DCE/scalar-replacement and causing
`assertRecoveredOnBailout` test failures. The same `FromExecutable` crash also
hit `disblic()` (the `DisassembleBaselineICs` testing function).

**The fix:** Added a `JitCode* jitCode_` field to `ICCacheIRStub`, set from the
constructor's `stubCode` parameter. Added `JitCode* jitCode() const` that shadows
`ICStub::jitCode()`. Now both JIT-pool stubs and static `.text` stubs return
their JitCode directly — no `FromExecutable` arithmetic needed.

Updated `ICCacheIRStub::trace()` to trace `jitCode_` directly instead of calling
the base class `jitCode()` with a static-code guard.

Removed the `WarpOracle::maybeInlineIC` early-return guard, allowing Ion to
transpile CacheIR from AOT stubs normally.

Fixed `DisassembleBaselineICs` (`disblic()`) in `TestingFunctions.cpp` to use
`cacheIRStub->jitCode()` instead of `stub->jitCode()` (the ICStub base version).

**Files changed:**
- `BaselineIC.h` — `jitCode_` field, constructor init, `jitCode()` method,
  updated size constants (4→5 words on 64-bit, 6→7 on 32-bit)
- `BaselineIC.cpp` — `trace()` uses `jitCode_` directly
- `WarpOracle.cpp` — Removed AOT static stub guard
- `TestingFunctions.cpp` — `disblic()` uses `cacheIRStub->jitCode()`

**Impact:** Eliminated 2 SIGSEGV failures (`cacheir/bug1937430.js`,
`fuses/1937176.js`) and 2 recover-on-bailout failures
(`ion/dce-with-rinstructions.js`, `ion/recover-rest-osr.js`).

### 3. `sLastCompileRelocs` Thread Safety Fix (Complete, Compiles)

**The problem:** `sLastCompileRelocs` is a static `Vector` used during AOT IC
stub dumping. `BaselineCacheIRCompiler::compile()` called `.clear()` on it
unconditionally (line 304), even at runtime when `isAOTFill()` is false. When
multiple worker threads compile IC stubs concurrently, they race on this shared
static Vector, triggering the `ReentrancyGuard` assertion `!mEntered`.

This affected tests that spawn many workers: `gc/bug-1565272.js`,
`ion/bug1394505.js`.

**The fix:** Moved `sLastCompileRelocs.clear()` inside the `if (masm.isAOTFill())`
guard. The vector is only used during AOT dump (single-threaded), so it should
only be touched in that context.

**Files changed:**
- `BaselineCacheIRCompiler.cpp` — `clear()` moved inside `isAOTFill()` guard

**Impact:** Should eliminate 2 ReentrancyGuard test failures.

## Current State

Before this session: 20 test failures.
After this session: 4 test failures (2 known/expected).

| Test | Status |
|------|--------|
| `baseline/blinterp-trial-inlining.js` | Known expected failure |
| `ion/recover-objects.js` | Known expected failure |
| `gc/bug-1565272.js` | Fixed (pending rebuild) |
| `ion/bug1394505.js` | Fixed (pending rebuild) |

## Design Decisions

1. **`callWithABI` timing**: Followed the existing pattern in
   `callWithABINoProfiler(Register)` — resolve argument moves first, then load
   the call target. `r10` (`ABINonArgReg2`) is safe after resolution because it's
   not an ABI argument register on SystemV x64.

2. **Storing JitCode* on ICCacheIRStub**: Adds 8 bytes per stub (one pointer),
   but eliminates all `FromExecutable` issues for static code. Considered
   alternatives (map lookup, side table) but storing directly is simplest,
   fastest, and correct by construction. The copy constructor propagates it
   naturally for `clone()`.

3. **WarpOracle guard removal**: With `jitCode()` working for static stubs,
   there's no reason to skip transpilation. Ion can now optimize through AOT IC
   sites normally, which is critical for `assertRecoveredOnBailout` tests and
   general Ion optimization quality.

## Files Modified (This Session)

1. `js/src/jit/MacroAssembler-inl.h` — `callWithABI` AOT intercept timing
2. `js/src/jit/BaselineIC.h` — `jitCode_` field + accessor + size constants
3. `js/src/jit/BaselineIC.cpp` — `trace()` simplified
4. `js/src/jit/WarpOracle.cpp` — AOT guard removed
5. `js/src/jit/BaselineCacheIRCompiler.cpp` — `sLastCompileRelocs` thread safety
6. `js/src/builtin/TestingFunctions.cpp` — `disblic()` uses `cacheIRStub->jitCode()`

## Remaining Known Issues

### `ion/recover-objects.js` (known expected)
The `assertRecoveredOnBailout` assertion still fires for this specific test.
The WarpOracle now transpiles AOT stubs, but `recover-objects.js` may exercise
a code path where the optimization still doesn't fire (possibly because the
specific IC stub involved isn't an AOT stub, or because Ion's scalar replacement
has additional constraints).

### `ICStub::jitCode()` on static code via `ICStub*`
`JitFrames.cpp:1192` calls `stub->jitCode()` through an `ICStub*` (base class
pointer). This still goes through `FromExecutable` and would crash for static
stubs. The existing `isStaticCode()` guard protects it. If new callsites through
`ICStub*` are added, they need the same guard — or `ICStub::jitCode()` itself
should be updated to handle static code (e.g., by storing the JitCode* on ICStub
instead of ICCacheIRStub).
