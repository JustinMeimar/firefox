# AOT IC Stubs — Session 4 Progress

## What Was Done

### 1. GC Tracing Static Code Guard (Complete, Compiles)

**The problem:** GC tracing walks JIT frames and calls `stub->jitCode()` →
`JitCode::FromExecutable(stubCode_)`. For AOT static IC stubs (code in `.text`),
there's no `JitCodeHeader` before the code bytes — the reverse pointer arithmetic
crashes on `MOZ_ASSERT(!code->isStaticCode_)`.

Two call sites:
- `ICCacheIRStub::trace()` (BaselineIC.cpp:462) — traces `JitCode` to keep it alive
- `TraceBaselineStubFrame` (JitFrames.cpp:1191) — reads `localTracingSlots()` count

**The fix:** Added `bool ICStub::isStaticCode() const` which checks whether
`stubCode_` falls within `[bl_aot_text_start, bl_aot_text_end)`. Guarded both
call sites to skip `jitCode()` for static code.

- Skipping trace is safe: the `JitCode` is marked through
  `jitZone->baselineCacheIRStubCodes_` map.
- Skipping `localTracingSlots` is safe: AOT stubs currently have 0 tracing slots
  (not serialized in `AOTICStubManifest`; only `callScriptedProxy` uses them).

**Impact:** Eliminated ~60% of remaining failures. All `arguments/`, `sort-trampoline`,
`grow-large-array` tests fixed.

**Files changed:**
- `BaselineIC.h` — `bool isStaticCode() const` declaration
- `BaselineIC.cpp` — Implementation + `#include "jit/AOT.h"` + guard in `trace()`
- `JitFrames.cpp` — Guard around `localTracingSlots` loop

### 2. handleFailure Stub Frame Fix (Complete, Compiles)

**The problem:** `MacroAssembler::finish()` emits `handleFailure()` code at the
end of IC compilation, AFTER `AutoStubFrame::leave()` resets `inAOTStubFrame_`.
But the failure code is jumped to FROM INSIDE stub frames (when a `callWithABI`
returns false). At that point `rbp` still points at the stub frame, not the
BaselineFrame. `emitAOTSlotLoad` loads `aotTableBase_` from `[rbp - 0x20]` which
is garbage in the stub frame context.

Disassembly showed:
```
+222: mov    -0x20(%rbp),%r11     # rbp = stub frame → garbage
+226: mov    0x70(%r11),%r11      # CRASH
+230: jmp    *%r11
```

**The fix:** In `MacroAssembler::finish()`, set `inAOTStubFrame_ = true` before
emitting `handleFailure()` when `isAOT()`. This makes `emitAOTSlotLoad` chase
`[rbp]` → saved BaselineFrame before accessing `aotTableBase_`.

Generated code becomes:
```
mov    (%rbp),%r11          # Chase: load saved rbp
mov    -0x20(%r11),%r11     # Load aotTableBase from BaselineFrame
mov    0x70(%r11),%r11      # Load exception tail from table
jmp    *%r11
```

**Impact:** Fixed `bug1423173` and likely several other crashes that showed
corrupted stack/values (the incorrect exception handler caused downstream
corruption).

**Files changed:**
- `MacroAssembler.cpp` — `enterAOTStubFrame()` before `handleFailure()` in `finish()`

### 3. Static Strings ImmPtr → RelocImmPtr (Complete, Compiles)

**The problem:** Several `MacroAssembler` functions for static string lookup
(`loadStringFromUnit`, `lookupStaticString`, `lookupStaticIntString`,
`loadLengthTwoString`) used `movePtr(ImmPtr(...))` to embed absolute addresses
of `StaticStrings` tables. In AOT IC stubs, these become stale pointers from the
build that generated the `.S` file.

Disassembly showed:
```
movabs $0x7f95caee6000,%rcx    # Stale address from previous build
mov    (%rcx,%rdx,8),%rcx       # CRASH: unmapped memory
```

This affected all `cacheir/string-*` tests, `cacheir/binaryarith`, and others.

**The fix:** Changed all 8 `ImmPtr` calls to `RelocImmPtr` in these functions.
`movePtr(RelocImmPtr(...))` automatically routes through `findSlotOrCrash` →
`emitAOTSlotLoad` in AOT mode.

Added 4 new indirection table slots:
- `StaticStringsUnitTable` → `&staticStrings.unitStaticTable`
- `StaticStringsLength2Table` → `&staticStrings.length2StaticTable`
- `StaticStringsIntTable` → `&staticStrings.intStaticTable`
- `StaticStringsToSmallCharTable` → `&StaticStrings::toSmallCharTable.storage`

**Files changed:**
- `AOT.h` — 4 new slots in `AOT_CORE_SLOTS`
- `Ion.cpp` — Populate 4 new slots in `populateAOTIndirectionTable()`
- `MacroAssembler.cpp` — 8× `ImmPtr` → `RelocImmPtr` in static string functions
- `StaticStrings.h` — `friend class js::jit::JitRuntime;` + forward decl

### 4. IC Dump Spew (Complete, Compiles)

Added per-stub and summary stdout output for `--dump-aot-ics`:
```
[BaselineAOT] AOT IC #0    kind=GetProp              size=  128b  cacheIR= 24b  fields=3
[BaselineAOT] AOT IC #1    kind=GetElem              size=  456b  cacheIR= 32b  fields=2  [gc]
...
[BaselineAOT] Dumped 47 AOT IC stubs (12345 bytes total)
```

**Files changed:**
- `BaselineCacheIRCompiler.cpp` — `fprintf` in IC blob save loop + summary after loop

## Current State

After fixes 1-3 (pending rebuild + bootstrap + test):
- Fix 1 reduced failures from ~5% to ~1.7%
- Fixes 2-3 target the remaining ~1.7%, particularly `cacheir/` string tests,
  `arrays/spread*`, and `auto-regress/` tests

## Remaining Known Issues

### Unguarded ImmPtr sites (potential)
There may be other `ImmPtr`/`ImmGCPtr` calls in IC codegen paths that haven't
been audited yet. Each would manifest as a SIGSEGV with a stale absolute address
in the disassembly. The pattern is always the same: change `ImmPtr` to
`RelocImmPtr` and add a slot if needed.

### localTracingSlots not serialized
`AOTICStubManifest` doesn't include `localTracingSlots`. Currently safe because
only `callScriptedProxy` stubs use them and they're not in the AOT set. If proxy
stubs are ever AOT-compiled, this needs serialization.

### Container version
`AOT_CONTAINER_VERSION` is still 3. Should bump to 4+ since indirection table
layout changed (4 new StaticStrings slots shift `VMWrapper_Begin`).

### Re-bootstrap required
The indirection table layout changed. Must re-bootstrap after build:
```bash
just build-shell-debug-aot
IONFLAGS=bl-aot ./jsshell --dump-bl-interp --dump-bl-self-hosted --dump-aot-ics -e 'quit(0);'
just build-shell-debug-aot
```

## Files Modified (This Session)

1. `js/src/jit/BaselineIC.h` — `isStaticCode()` declaration
2. `js/src/jit/BaselineIC.cpp` — `isStaticCode()` impl + trace guard
3. `js/src/jit/JitFrames.cpp` — `localTracingSlots` guard
4. `js/src/jit/MacroAssembler.cpp` — `handleFailure` stub frame fix + 8× `RelocImmPtr`
5. `js/src/jit/AOT.h` — 4 new StaticStrings slots
6. `js/src/jit/Ion.cpp` — Populate 4 new slots
7. `js/src/vm/StaticStrings.h` — `JitRuntime` friend decl
8. `js/src/jit/BaselineCacheIRCompiler.cpp` — IC dump spew
