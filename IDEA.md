# Super-stubs

Fuse a region of bytecode ops within a basic block into a single IC artifact
(a super-stub), eliding per-op IC dispatch, value-stack traffic between ops,
and redundant guards. Idea 1 (guarded devirtualization) generalized along the
bytecode axis: GD is n stubs at one op, a super-stub is one stub per op across
k ops. Matt's inline-stub design is the k=1 case.

References: StitchMonkey doc (Gaudet, 2026-11-07), Iain's recipe histogram
(gist 1be3ef92), GD microbenches (gist efe8e918), AmberMonkey CGO paper
(recurrence: IC stubs ~full, guest Baseline functions 4.9%).

## Two separate angles

- **Run-time.** No recurrence needed. Any effect-safe region observed in a
  block can be compiled at attach time. Questions: effect model, attach and
  detach policy, feedback starvation, compile cost.
- **AOT.** Needs a finite precompiled set, so recurrence matters. The open
  measurement is the recurrence curve between k=1 (IC stub) and the whole
  function: how much survives at k=2,3,4..., and at k=|BB|, and what kills it
  (control flow, barriers, recipe-tuple blowup).

## Region rules

- A region lies within one basic block (no jump targets or loop heads inside).
- Invariant: **no guard after the first effect.** A region is a pure prefix
  (guards anywhere, e.g. int32 overflow in an arithmetic chain) followed by an
  effectful suffix whose guards are hoisted ahead of it.
- On guard failure nothing has been committed: discard and run the unfused
  per-op path from region entry. No mid-region resume PC, no bailout machinery.
- Barriers end a region: anything that can run script, GC, or change a shape
  (calls, scripted/native getters and setters, proxies, valueOf, property
  add/delete). Calls being barriers makes "Call is diffuse" a non-issue.

## Tier agnosticism

The super-stub is an IC-level artifact attached in the ICScript and entered
from the head op's IC site. It leaves the expression stack as after k ops and
reports how many ops it consumed. The only tier seam is the continuation:

- blinterp: advance pc by k and dispatch.
- Baseline: blinterp attaches regions before Baseline compiles, so Baseline can
  place labels at region ends. Late-attached regions need a pc to native map.

## Patterns are programmatic

At run time there is no pattern catalogue. A super-stub is the concatenation
of constituent CacheIR with stack-slot operands rewired to SSA values, then a
guard dedupe and hoist pass (a small region compiler). Arbitrary-depth
arithmetic trees fall out for free. Pattern families (e.g. right-associative
int32 chain of depth <= N) matter only for AOT, where one parametric body can
cover many sites.

## Effect model

Derive, do not declare: transpilable CacheIR ops lower to MIR with alias sets,
and Ion already hoists/dedupes guards over these same ops with GVN/LICM on that
basis. Gaps outside alias sets:

- [ ] GC: Ion has safepoints, a super-stub does not. Ops that can GC (VM or
      ABI calls, roughly cost_estimate >= 4) are barriers, or spill around them.
- [ ] Fuses: Warp uses compile dependencies and invalidation, not alias sets.
      Need a rule for a hoisted fuse guard followed by a store that could pop
      it (model the fuse as a load clobbered by slot/element stores).
- [ ] Bailouts: replaced by the commit-point rule.
- [ ] Non-transpilable ops have no derived effects: barriers by default.
      Measure their share of hot coverage.

## Feedback starvation

If the fused path skips per-op ICs, Warp and trial inlining read stale
counts and stub chains.

- Count on the super-stub; ICScript readers attribute hits to constituent ops
  (cost on the cold reader, not the hot path). Alternative: bump each
  constituent counter (k stores per hit).
- Warp could transpile super-stubs directly (they are CacheIR) and start from
  already-hoisted guards.
- Staleness is benign (guards still check); needs a detach policy when a
  constituent op goes polymorphic.

## Where cycles go (measure separately)

1. IC dispatch: indirect call + return per op (known from GD microbenches).
2. Value-stack traffic: box, store, load, re-check tag in the next stub.
   Suspected larger than guard elision.
3. Redundant guards: repeated GuardToObject/GuardShape on the same value with
   no barrier between. A predicted guard on a hot shape is ~1 cycle, so do not
   let the story hinge on this without the oracle bound.

## First step: tracing, no codegen

- [ ] Per IC hit in blinterp: (script, pc, recipe hash). Check what aot-record
      already logs.
- [ ] Per op: barrier flag from derived alias sets plus the GC and fuse rules.
      Quick sanity check: what share of Iain's histogram is barrier-free.
- [ ] Offline (streamed): split into basic blocks, symbolic stack simulation
      for dataflow, canonicalize (alpha-rename slots and atoms).
- [ ] Report plain counts:
  - maximal effect-safe regions per block (run-time unit)
  - canonical region forms with train/held-out recurrence (AOT unit)
  - whole-block recurrence for comparison
  - savings breakdown: dispatches, stack round-trips, guards (oracle bound)
- [ ] Look for programmatic families as clusters among canonical forms.

## Prior art to position against

- CPython 3.13+ tier 2: specialized micro-op traces, type-version guard
  elimination, copy-and-patch. Closest; differentiate on CacheIR recipe
  richness, derived effect model, AOT recurrence, shared ICScript.
- Proebsting, superoperators (POPL '95); Ertl and Gregg, superinstructions.
- Brunthaler, Inline Caching meets Quickening (ECOOP '10).
- Xu and Kjolstad, Copy-and-Patch (OOPSLA '21), super-stencils.
- HHVM tracelets (OOPSLA '14, PLDI '18); lazy BBV (ECOOP '15).
