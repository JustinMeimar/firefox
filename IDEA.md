# IC Region Versioning

A region of bytecode ops within a basic block gets its own IC site. Each stub
on that site is a version: a clone of the region compiled from the CacheIR
already attached at its ops, plus the frame-local ops between them. The
fallback is the existing per-op path. Attach, polymorphism, megamorphic
give-up and detach are the IC mechanics SpiderMonkey already has, lifted from
op to region.

What a version removes:

1. IC dispatch: one call and return per constituent op.
2. Value-stack traffic: box, store, load, re-check tag between ops. Frame-local
   ops (GetLocal, SetLocal, Dup, Swap, Pop, constants) become register moves or
   vanish. Expected to dominate.
3. Redundant guards: deduped and hoisted to region entry where their operand
   exists at entry. A predicted guard is ~1 cycle; the story does not hinge on
   this.

References: StitchMonkey doc (Gaudet, 2026-11-07), Iain's recipe histogram
(gist 1be3ef92), GD microbenches (gist efe8e918), AmberMonkey CGO paper
(recurrence: IC stubs ~full, guest Baseline functions 4.9%).

## The region IC

- **Site.** The region head op. A region lies within one basic block (no jump
  targets or loop heads inside) and ends at a barrier.
- **Stubs are versions.** Entry guards cover values live at entry (stack
  inputs, locals, `this`). Guards on values produced inside the region stay in
  place.
- **Fallback is the per-op path.** Ordinary Baseline code (or blinterp
  dispatch) for the block, with each op's own IC chain. It is also the feedback
  source: a new stable tuple of per-op stubs is what attaches a new version.
- **Polymorphism.** Versions chain like stubs. After N versions the site goes
  generic and stays on the per-op path.
- **Detach.** A version is dropped when a constituent op's chain changes under
  it (a stub it was built from is discarded, or the op goes megamorphic).

## Versions are clones, not fused stubs

A version is the whole region: constituent CacheIR with stack-slot operands
rewired to SSA values, the frame-local ops in between, then a guard dedupe and
hoist pass. There is no pattern catalogue at run time; arbitrary-depth
arithmetic trees fall out.

Code and data split as in Baseline stubs: version code is keyed on the
canonical region form and shared (JitZone stub cache), per-site fields
(shapes, slot offsets) live in a stub data blob concatenated from the
constituents. Code size scales with distinct forms, not sites. The same key is
the AOT unit.

## Exits

- **Entry guard failure.** Nothing ran: take the next version or the fallback.
- **Mid-region failure.** Side exit: spill live SSA values to their
  expression-stack slots and continue at the failing op's pc. Baseline jumps to
  that pc's native address (pc mapping table, as OSR and debug mode use);
  blinterp sets pc and dispatches. Same frame type on both sides, so this is
  not a bailout and needs no snapshots.
- **Region end.** Continue at the end pc the same way.

Side exits remove the commit-point restriction: a failure after an effect
resumes at that op instead of replaying the region. Regions cut only at
barriers.

## Effect model

At the JSOp level nearly everything can run user code (valueOf,
Symbol.toPrimitive, getters, setters, proxy traps). An attached stub either
guards those paths out or calls them explicitly, and every stub that can run
script or GC enters a stub frame. So "can this run user code" is a mechanical
per-recipe fact, not a hand-written node annotation (the TurboFan
CVE-2018-17463 class).

- **Barriers:** stub frame entered (VM or script call, can GC), MIR
  possiblyCalls, non-transpilable ops, fallback.
- **Strict:** no guard hoisted across a store. Sound given the barrier bit.
- **Alias:** hoist across stores with disjoint MIR alias sets. Inherits the
  trust Ion's GVN/LICM already place in hand-declared alias sets.
- [ ] Derive effects per recipe at attach time instead of the corpus union.
- [ ] Fuses: model a fuse guard as a load clobbered by slot/element stores
      (alias rule only).
- [ ] Hidden writes (rope flattening, megamorphic cache fills): confirm none
      invalidate a hoisted guard.
- [ ] Debugger: no versions in debuggee frames.

## Feedback

The per-op ICs only see fallback executions, so Warp and trial inlining read
stale counts and chains.

- Count on the version; ICScript readers attribute hits to constituent ops
  (cost on the cold reader, not the hot path).
- Warp can transpile versions directly (they are CacheIR) and start from
  hoisted guards.
- Staleness is benign: guards still check.

## Tiers

- blinterp: the head op's handler checks for an attached region site (one
  load and a branch); a version ends by setting pc and dispatching.
- Baseline: region sites attached before Baseline compiles become direct entry
  points; exits use the pc map.

## Toward BBV

Versions with entry guards and a retranslation limit are close to HHVM
tracelets. BBV proper adds context propagation to successors: if block B's
entry guards are already proven by block A's version, A can jump straight to
B's matching version. The IC system supplies that context too, which is the
claim: block versioning falls out of a structured IC system.

## Measurement

Shell flag `--ic-trace=PATH` (`%p` expands to the pid); format in
js/src/jit/ICTrace.cpp. Tools in super-stubs/: `agg/` (Rust aggregator),
`ictrace.py`, `dataflow.py`, `regions.py`.

- [x] Per IC execution: stub entry, guard failure, fallback, attach, clone,
      per-script bytecode. Hit = last entered stub without a failure.
- [x] Per CacheOp effects: MIR nodes and alias sets Warp emits ('T'/'M'),
      stub frame entered ('B').
- [x] Basic-block split, symbolic stack simulation, guard canonicalization.
- [x] Octane regions, strict and alias: dispatches saved, IC inputs fed by
      earlier ICs, repeated guards, guards on region values.
- [ ] Versions per site: share of block executions on the dominant stub tuple,
      versions needed for 95%.
- [ ] Share of runtime spent in blinterp and Baseline.
- [ ] Canonical region forms with train/held-out recurrence (AOT unit).
- [ ] Non-IC JSOps: frame-local whitelist in regions.py is declared, not
      derived.

## Prior art to position against

- HHVM tracelets and retranslation chains (OOPSLA '14, PLDI '18). Closest on
  versioning.
- Lazy BBV (Chevalier-Boisvert and Feeley, ECOOP '15); YJIT.
- CPython 3.13+ tier 2: specialized micro-op superblocks, type-version guard
  elimination, copy-and-patch.
- Proebsting, superoperators (POPL '95); Ertl and Gregg, superinstructions.
- Brunthaler, Inline Caching meets Quickening (ECOOP '10).
- Xu and Kjolstad, Copy-and-Patch (OOPSLA '21), super-stencils.

Differentiators: no separate type profile (a version is keyed on the attached
stubs), code/data split preserved (shareable, AOT-able), derived effect model,
no new deopt machinery (fallback is the per-op path, exits use the pc map).
