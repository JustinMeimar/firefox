# AOT IC Stubs — Session 2 Progress

## What Was Done

### 1. ImmGCPtr → Indirection Table (Complete, Compiles)
Added `AtomEmpty`, `AtomTrue`, `AtomFalse` to `AOT_CORE_SLOTS` and routed
the following codegen sites through `emitAOTSlotLoad` in AOT mode:

| Site | File | What |
|------|------|------|
| `emitArrayJoinResult` | CacheIRCompiler.cpp:7525 | `names().empty_` |
| `emitBooleanToString` | CacheIRCompiler.cpp:10169,10181 | `names().false_`, `names().true_` |
| `loadFunctionName` | MacroAssembler.cpp:5988 | `emptyString` |
| `emitLoadStringCharResult` | BaselineCacheIRCompiler.cpp:1187 | `names().empty_` |
| `useRegister` Constant | CacheIRCompiler.cpp:355 | `MOZ_CRASH` guard |

Ion.cpp: populated the 3 new slots with `static_cast<JSString*>(cx->names().X)`.

### 2. callVM → Indirection Table (Complete, Compiles)
`CacheIRCompiler::callVMInternal()` (line ~11997) now has an AOT guard:
```cpp
if (masm.isAOT()) {
    ScratchRegisterScope scratch(masm);
    AOTSlot slot = AOTSlotForVMWrapper(uint32_t(id));
    masm.emitAOTSlotLoad(slot, scratch);
    masm.push(FrameDescriptor(FrameType::BaselineStub));
    masm.call(scratch);
    return;
}
```
This eliminates stale `rel32` jumps to JIT-pool VM wrapper trampolines
(`codeJumps_` entries) by routing through the existing `VMWrapper_Begin..End`
indirection table slots.

### 3. callWithABI → `.quad <symbol>` via dladdr (Partial — blocked on local symbols)

**Infrastructure built:**
- `AOTCodeReloc` struct + `codeRelocs` field on `AOTBlobData` (BaselineAOT.h)
- `compile()` captures `extendedJumps()` reloc info after `masm.finish()`
  (offset = `extendedJumpTableOffset() + i * sizeOfJumpTableEntry() + 8`)
- `FillAOTICs()` copies relocs into the blob
- `.S` writer splices `.quad <symbol>` at reloc offsets via `dladdr()`
- Added public accessors to `Assembler-x64.h`: `extendedJumps()`,
  `extendedJumpTableOffset()`, `sizeOfJumpTableEntry()`

**The problem:** `dladdr()` fails for ~8 C++ functions because they have
**local linkage** (lowercase `t` in `nm`). These are functions like:

| Function | Notes |
|----------|-------|
| `js::jit::AssertPropertyLookup` | DEBUG-only |
| `js::jit::EqualStringsHelperPure` | |
| `js::jit::PostWriteElementBarrier` | |
| `js::jit::LinearizeForCharAccessPure` | |
| `js::jit::ObjectIsCallable` | |
| `js::jit::ObjectIsConstructor` | |
| `js::jit::TypeOfNameObject` | |
| `js::NativeObject::addDenseElementPure` | |

Current fallback: emits raw pointer bytes (will be stale across rebuilds).

## Files Modified

1. `js/src/jit/AOT.h` — 3 new `AOT_CORE_SLOTS` entries
2. `js/src/jit/Ion.cpp` — 3 new `SET()` lines for atoms
3. `js/src/jit/CacheIRCompiler.cpp` — ImmGCPtr guards (3 sites) + callVM AOT intercept
4. `js/src/jit/MacroAssembler.cpp` — loadFunctionName AOT guard
5. `js/src/jit/BaselineCacheIRCompiler.cpp` — emitLoadStringCharResult guard + extendedJumps capture + reloc passing
6. `js/src/jit/BaselineAOT.h` — `AOTCodeReloc` struct, `codeRelocs` on `AOTBlobData`
7. `js/src/jit/BaselineAOT.cpp` — `#include <dlfcn.h>`, `emitAsmBytesWithRelocs()`, `.quad` emission
8. `js/src/jit/x64/Assembler-x64.h` — 3 public accessors

## Remaining Work / Uncertainties

### Blocker: Local-linkage C++ functions
The 8 functions above have `static` or internal linkage — `dladdr` can't
resolve them to symbol names. Options to fix:

1. **Remove internal linkage** from these ~8 functions (add `JS_PUBLIC_API`
   or just remove `static`). They're already in the `js::jit` namespace so
   name collision risk is minimal. This is probably the simplest fix.

2. **Build a manual registry** mapping `void*` → mangled name string at
   dump time. Could use `__FUNCTION__` or a macro at the `callWithABI`
   call site.

3. **Use `nm` output** at dump time: shell out to `nm` on the binary,
   parse the output, and look up addresses. Fragile but works.

### Other potential issues
- **Pre-barrier calls** from IC stubs — these could produce `codeJumps_`
  entries to JIT-pool pre-barrier trampolines. The indirection table already
  has `PreBarrier_*` slots, but IC stub codegen may not use them in AOT mode.
  Needs investigation.

- **Other `codeJumps_` targets** — any `call(JitCode*)` or `call(TrampolinePtr)`
  in IC stubs that isn't `callVM` would also produce stale `rel32` entries.
  The `callVM` fix only handles one path.

- **Container version** — `AOT_CONTAINER_VERSION` is still 3. Should bump
  to 4 since slot layout changed (3 new slots shift `VMWrapper_Begin`).

## Bootstrap State
The `.S` file was dumped successfully with the dladdr fallback (raw pointers
for the 8 local-linkage functions). A full bootstrap cycle has not been
completed past this point — the rebuild with the new `.S` and subsequent
test run is pending.

## Recommended Next Step
Fix the 8 local-linkage functions by removing `static` / adding visibility,
then complete the bootstrap cycle and run jit-tests.
