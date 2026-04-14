# AOT IC Stubs: Progress & Next Steps

## What Was Done (this session)

### Zone Register Removal (committed)
Stripped Chase's zone-register approach from IC codegen and replaced it with
the `RelocImmPtr` / `AOTIndirectionTable` pattern used by blinterp.

**Before:** Each `ICStub` carried a `CompileZone* zone_` field. At IC entry,
`loadZone()` pinned `HeapReg` (r13) to the zone pointer. All zone-relative
accesses (nursery, free lists, preserved wrappers, incremental barrier flag)
went through `Address(zoneReg(), Zone::offset...)`.

**After:** Zone access goes through `loadZoneForAOT(dest)`:
```
loadJSContext(dest)          // RelocImmPtr -> emitAOTSlotLoad(JSContextPtr)
loadPtr(dest + offsetOfZone) // JSContext::offsetOfZone()
```
This is 3-4 loads vs the old 1 load from the pinned register, but eliminates
the per-stub zone pointer, the dedicated register, and all `*Runtime()` method
variants.

**Files changed:**
- `MacroAssembler.cpp` — collapsed `isAOTFill()` branches in `loadJSContext`,
  `loadMegamorphicCache`, `loadMegamorphicSetPropCache`,
  `loadAtomOrSymbolAndHash` (StringToAtomCache); converted zone accesses in
  `freeListAllocate`, `bumpPointerAllocateRuntime`, `preserveWrapper`;
  removed `zoneReg()`, `loadZone()`
- `AOTMacroAssembler.cpp` — added `loadZoneForAOT()`
- `MacroAssembler.h` — removed `zoneLoaded_`, `zoneReg()`, `loadZone()` decls;
  added `loadZoneForAOT` decl
- `MacroAssembler-inl.h` — removed `branchTestNeedsIncrementalBarrierRuntime`
- `BaselineIC.h` — removed `zone_` field, `offsetOfZone()`, zone from constructors;
  fixed size constants
- `BaselineCacheIRCompiler.cpp` — removed `loadZone()` calls, zone from stub
  creation, added IC frame pointer assert
- `SharedICRegisters-x64.h` — removed `ZoneReg`
- `SharedICHelpers-*.h` (all 7 archs) — removed `loadZone()` after stub frame leave
- `BaselineIC.cpp` — removed `regs.take(ZoneReg)`

### Indirection Table Alignment (committed)
- Changed `loadMegamorphicCache`/`loadMegamorphicSetPropCache` from `ImmPtr` to
  `RelocImmPtr` so they go through `findSlotOrCrash` → `emitAOTSlotLoad` in AOT mode.
- Fixed `StringToAtomCache` slot: table was storing `base + offsetOfLastLookups()`,
  but `RelocImmPtr` passes just the base. Changed `Ion.cpp` table population to
  store the base; codegen now does `emitAOTSlotLoad` + `computeEffectiveAddress`.
- Fixed `DumpAOTContainer` assert to accept standalone `--dump-aot-ics`.
- Fixed `LoadAOTICStubs` to skip duplicate entries (same CacheIR key) rather than
  asserting.

### Verification
- `--dump-aot-ics` successfully compiles 47+ IC stubs using `FillAOTICs` (JIT
  compilation from CacheIR bytecode with `isAOTFill=true`). The compiled code
  uses `emitAOTSlotLoad` for all runtime pointers.
- `--aot-bl` without binary IC stubs passes all jit-tests (ICs are JIT-compiled
  at startup via `FillAOTICs`).
- `--aot-bl` WITH binary IC stubs loaded from the container **crashes** — the
  loaded native code has unpatched relocations.

---

## The Blocking Problem: IC Stub Relocations

### How blinterp relocations work

The blinterp blob is position-independent at the instruction level: all runtime
pointers go through `emitAOTSlotLoad` (2-load indirection from `FramePointer`).
But it still has **internal relocations** — PC-relative references to other
positions within the blob (jump targets, branch offsets) that are valid at the
compile-time code address but not at the load-time address.

The blinterp serialization records these in an `AOTRelocEntry` table. At load
time, `ApplyAOTRelocations` patches each entry: `newAddr = oldAddr - oldBase +
newBase`. This is how `AOTBaseline.S` works — the `.S` file embeds raw bytes +
a relocation table, and the loader applies fixups.

### Why IC stubs crash

IC stubs compiled via `BaselineCacheIRCompiler` with `isAOTFill=true` use the
same `emitAOTSlotLoad` pattern for runtime pointers (good). But the native code
also contains:

1. **VM call wrappers** — `loadVMWrapper(id, dest)` uses
   `movePtr(RelocImmPtr(ptr.value), dest)` which becomes `emitAOTSlotLoad`.
   These should be fine.

2. **Jump/branch targets within the stub** — these are PC-relative on x64 and
   should be position-independent. Probably fine.

3. **Stub data field addresses** — IC stubs reference `ICStubReg + offset` for
   inline data. These are register-relative, not absolute. Fine.

4. **Tail calls to next stub / fallback** — uses `ICTailCallReg`. Should be
   register-relative. Fine.

5. **Return address / exception tail** — `handleFailure()` uses
   `movePtr(RelocImmPtr(excTail.value), ScratchReg)` → `emitAOTSlotLoad`. Fine.

6. **`ImmPtr` calls that bypass `RelocImmPtr`** — any code path that uses
   `ImmPtr` (not `RelocImmPtr`) will embed an absolute address that won't be
   intercepted by the AOT override. These become **stale pointers** when the
   code is loaded at a different address. This is the likely crash cause.

### What needs to happen

**Phase 1: Audit `ImmPtr` usage in IC codegen paths**

When `isAOTFill()` is true, identify every `ImmPtr` / `ImmGCPtr` /
`AbsoluteAddress` that embeds a compile-time pointer into the machine code.
These are the relocations that need recording.

Key files to audit:
- `CacheIRCompiler.cpp` — the bulk of IC op implementations
- `BaselineCacheIRCompiler.cpp` — baseline-specific IC ops
- `MacroAssembler.cpp` — helpers called during IC compilation

**Phase 2: Record relocations during IC compilation**

The `MacroAssembler` already has relocation tracking for blinterp
(`aot().addReloc(...)` or similar). Extend this to IC stubs:

- During `BaselineCacheIRCompiler::compile()`, track each absolute pointer
  emission as a relocation entry.
- After compilation, extract the relocation table alongside the code bytes.

**Phase 3: Serialize relocations into the container**

The `AOTICStubManifest` needs a relocation table field. When `dumpAOTICs`
serializes a stub blob, include the relocation entries in the metadata section.

**Phase 4: Apply relocations at load time**

`LoadAOTICStubs` needs to:
1. Allocate executable memory for the IC code at a runtime-determined address.
2. Read the relocation table from the manifest.
3. Apply `newAddr = oldAddr - oldBase + newBase` for each relocation.
4. The code is now valid at its new address.

### Alternative: Eliminate `ImmPtr` from IC codegen entirely

Instead of building a relocation table, convert every remaining `ImmPtr` in IC
codegen to `RelocImmPtr` (which goes through the indirection table and is
position-independent by construction). This is what we did for `loadJSContext`,
`loadMegamorphicCache`, etc.

This is the cleaner long-term approach but requires:
- Adding AOT slots for every pointer IC code needs
- Auditing that no `ImmPtr` / `AbsoluteAddress` calls remain in IC paths
- The indirection table is finite (currently 512 VM wrapper slots + ~30 core
  slots); verify it's large enough.

**Recommendation:** Hybrid approach. Convert the common `ImmPtr` calls to
`RelocImmPtr` (like we already did), then use a small relocation table for any
remaining edge cases. Start by auditing what `ImmPtr` calls actually appear in
IC-compiled code.

---

## Key Decisions Made

1. **Zone access via JSContext, not pinned register.** 3-4 loads per zone
   access, but no per-stub storage and no register pressure.

2. **`RelocImmPtr` for all shared runtime pointers.** Megamorphic caches,
   StringToAtomCache, JSContext, VM wrappers all go through the indirection
   table. No `loadRuntime + offset` fallback for IC fill.

3. **`isAOTFill()` guards kept only for compile-time-only checks.** Guards for
   `geckoProfiler().enabled()`, realm fuses, `hasRealmWithAllocMetadataBuilder`,
   GC zeal, etc. remain — these skip code that can't execute in AOT mode.

4. **`emitAOTSlotLoad` uses `FramePointer`** — IC frame pointers must be
   disabled in AOT mode (asserted in `compile()`). `FramePointer` must point at
   the caller's `BaselineFrame`, not an IC frame.

5. **Duplicate IC stubs in the container are tolerated on load** — `LoadAOTICStubs`
   skips entries whose CacheIR key already exists in the hashmap.

## Build/Test Commands

```bash
# Build
just build-shell-debug-aot

# Bootstrap (generate AOT container with ICs)
IONFLAGS=bl-aot ./jsshell --dump-bl-interp --dump-bl-self-hosted --dump-aot-ics -e 'quit(0);'

# Rebuild with container
just build-shell-debug-aot

# Test (without binary IC stubs — ICs JIT-compiled at startup)
IONFLAGS=bl-aot ./jsshell --aot-bl -f ../tests/debug-ion.js
cd js/src && python3 ./jit-test/jit_test.py --args="--aot-bl" ../../jsshell

# Test (with binary IC stubs — CURRENTLY CRASHES, needs relocation support)
# The above tests use FillAOTICs (JIT compile from CacheIR) which works.
# LoadAOTICStubs (load binary from container) loads but stubs crash on execution.
```
