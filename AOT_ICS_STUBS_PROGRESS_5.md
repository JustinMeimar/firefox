# AOT IC Stubs — Session 5 Progress

## Audit: CacheIR Codegen for Embedded Absolute Addresses

Performed a comprehensive audit of `CacheIRCompiler.cpp`, `BaselineCacheIRCompiler.cpp`,
and `MacroAssembler-inl.h` for patterns that embed absolute addresses into IC stub code.

### Already Handled (Safe)

| Pattern | Why Safe |
|---------|----------|
| `callWithABI<Fn, target>()` (template form, ~50 sites) | Intercepted at `MacroAssembler-inl.h:154` via `findSlotOrCrash` |
| `callVM` / VMWrapper | AOT guard loads slot via `AOTSlotForVMWrapper` |
| `ImmGCPtr(names.empty_)` | Guarded by `isAOT()` → `emitAOTSlotLoad(AtomEmpty)` |
| `ImmGCPtr(names.true_/false_)` | Guarded by `isAOT()` → `emitAOTSlotLoad(AtomTrue/AtomFalse)` |
| `OperandLocation::Constant` ImmGCPtr | `MOZ_CRASH` guard for AOT |
| `emitLoadStubFieldConstant` (ImmGCPtr) | Ion-only (`MOZ_ASSERT(mode_ == Mode::Ion)`) |
| `lookupStaticIntString` | Already uses `RelocImmPtr` |
| `loadMegamorphicCache` / `loadMegamorphicSetPropCache` | Already uses `RelocImmPtr` |
| `callWithABI(Address(...))` (indirect call via register) | No embedded pointer |
| `ImmPtr(nullptr)` / `ImmWord(0/1)` | Just integer constants |
| `movePropertyKey` with baked PropertyKey | Only from Ion-only `emitMegamorphicCacheLookup` |

### Found & Fixed

#### 1. `callWithABI(DynFn, ...)` — DynamicFunction Intercept

**Problem:** The `DynamicFunction<T>` overload of `callWithABI` was NOT intercepted for
AOT. It goes through `callWithABINoProfiler(fun.address, ...)` which calls
`call(ImmPtr(fun))` — embedding a raw function pointer.

Affected call sites in `CacheIRCompiler.cpp`:
- `GetUnaryMathFunctionPtr(fun)` — math functions (sin, cos, etc.)
- `BigIntEqual<...>` / `BigIntCompare<...>` — BigInt comparison
- `AtomicsCompareExchange(elementType)` — atomics compare-exchange
- `AtomicsReadWriteModifyFn(fn)` — atomics read-write-modify

**Fix:** Added AOT intercept to `callWithABI(DynFn, ...)` in `MacroAssembler-inl.h` —
same `findSlotOrCrash` pattern as the template form. Also:
- Added `BigIntEqual`/`BigIntCompare` (4 instantiations) to `ABIFUNCTION_LIST`
- Populated math functions (26 variants via `GetUnaryMathFunctionPtr` loop) in
  indirection table
- Populated atomics functions (42 variants: 7 ops × 6 types) in indirection table

**Files:** `MacroAssembler-inl.h`, `ABIFunctionList-inl.h`, `Ion.cpp`

#### 2. `DeadObjectProxy::singleton` — Static Pointer in IC Stub

**Problem:** `BaselineCacheIRCompiler::emitGuardCompartment` embeds
`ImmPtr(&DeadObjectProxy::singleton)` — a static address that shifts with ASLR
between builds.

**Fix:** AOT guard loads from existing `AOTSlot::DeadObjectProxySingleton` slot and
uses register-register comparison instead.

**File:** `BaselineCacheIRCompiler.cpp`

#### 3. `moveValue(StringValue(cx_->names().X))` — typeof Name Atoms

**Problem:** `emitLoadTypeOfObjectResult` bakes GC pointers to name atoms directly
into code via `moveValue(StringValue(...))` for three typeof results:
- `cx_->names().function` (line 8393)
- `cx_->names().undefined` (line 8397)
- `cx_->names().object` (line 8401)

**Fix:** Added three new AOT slots (`AtomFunction`, `AtomUndefined`, `AtomObject`),
populated in `Ion.cpp`, and guarded in `CacheIRCompiler.cpp` with
`emitAOTSlotLoad` + `tagValue`.

**Files:** `AOT.h`, `Ion.cpp`, `CacheIRCompiler.cpp`

#### 4. `branchTestClassIsFunction` — FunctionClass/ExtendedFunctionClass Pointers

**Problem:** `MacroAssembler::branchTestClassIsFunction` embeds
`ImmPtr(&FunctionClass)` and `ImmPtr(&ExtendedFunctionClass)` — static addresses
in the binary that shift with ASLR. This is called from `typeOfObject` and many
guard paths.

**Fix:** AOT guard uses existing `Class_Function` and `Class_ExtendedFunction`
indirection table slots via `emitAOTSlotLoad` + register comparison.

**File:** `MacroAssembler-inl.h`

## Test Status

Before this session: ~1.7% failure rate (~22 failures).
After rebuild (pre-bootstrap): same rate — expected since stubs need re-dumping.
Bootstrap required to see impact of these fixes.

## Remaining Suspects

- **Map/Set hash assertion failures** (`actualHash == HashValue(...)` at
  VMFunctions.cpp:3280/3287) — seen in set-has-value.js, map-get-string.js, etc.
  These are debug-only assertions. The hash computation itself doesn't embed pointers;
  the mismatch may be GC-related (stale values in AOT stubs) rather than embedded
  addresses.

- **Ion test failures** (new-2/4/5/6.js, bug885660.js, etc.) — likely pre-existing
  or related to Ion's interaction with AOT stubs. The WarpOracle guard at line 1055
  already skips inlining AOT stubs.

- **`branchTestProxyHandlerFamily`** — embeds `&Wrapper::family` as `ImmPtr`, but
  only reachable from `branchIfObjectEmulatesUndefined` (interpreter path, not IC
  stubs). Lower priority.

## Files Changed This Session

- `js/src/jit/MacroAssembler-inl.h` — DynFn AOT intercept + branchTestClassIsFunction guard
- `js/src/jit/BaselineCacheIRCompiler.cpp` — DeadObjectProxy::singleton guard
- `js/src/jit/ABIFunctionList-inl.h` — BigIntEqual/BigIntCompare additions
- `js/src/jit/Ion.cpp` — math/atomics table population + new atom slots + jsmath.h include
- `js/src/jit/AOT.h` — AtomFunction, AtomUndefined, AtomObject slots
- `js/src/jit/CacheIRCompiler.cpp` — typeof atom AOT guards

## Notes

### sccache

sccache is maxed at 10 GiB (default). Attempted to increase to 20 GiB via
`SCCACHE_CACHE_SIZE = "20G"` in `mozconfigs/spidermonkey.nix` but the env var
is not being exported by the flake devshell — the flake.nix likely imports
the shell differently. Need to find where the flake defines `devShells.spidermonkey`
and add the env var there, or set it in `.envrc` directly:

```
export SCCACHE_CACHE_SIZE="20G"
```

After setting, run `sccache --stop-server` so the new size takes effect.
