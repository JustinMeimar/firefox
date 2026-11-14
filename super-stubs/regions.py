#!/usr/bin/env python3
"""Cut basic-block instances into fusible regions and report plain counts.

  regions.py classify MIR.pkl
  regions.py report AGG.pkl MIR.pkl

AGG.pkl comes from `ictrace.py aggregate`, MIR.pkl from `ictrace.py mir`.

A CacheOp's effects are the union of what Warp emitted for it over the MIR
corpus. Ops that were never transpiled there are opaque. A recipe is a barrier
if any of its ops enters a stub frame in the baseline stub compiler (VM or JIT
call, can GC or run script), calls in MIR, is opaque, or (under --gc=cost) has
a cost estimate of at least 4 in CacheIROps.yaml.

Two cut rules:
  strict: a region ends after its first effectful IC.
  alias:  an IC whose guards' read set does not intersect the region's store
          set may follow a store (ignores dataflow into the guards).
"""

import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(__file__))
import ictrace  # noqa: E402

STORE = 1 << 31
STRUCTURAL_OPS = {"ReturnFromIC"}

# Interpreter ops without ICs that only touch the frame or push constants.
FRAME_LOCAL_JSOPS = {
    "Nop",
    "Pop",
    "PopN",
    "Dup",
    "Dup2",
    "DupAt",
    "Swap",
    "Pick",
    "Unpick",
    "GetLocal",
    "SetLocal",
    "GetArg",
    "GetAliasedVar",
    "GetRval",
    "SetRval",
    "Undefined",
    "Null",
    "True",
    "False",
    "Zero",
    "One",
    "Int8",
    "Uint16",
    "Uint24",
    "Int32",
    "Double",
    "String",
    "Symbol",
    "Void",
    "JumpTarget",
    "LoopHead",
    "CheckLexical",
    "CheckAliasedLexical",
    "IsNullOrUndefined",
    "StrictConstantEq",
    "StrictConstantNe",
    "Goto",
    "JumpIfFalse",
    "JumpIfTrue",
    "And",
    "Or",
    "Coalesce",
    "NopIsAssignOp",
    "IsConstructing",
    "BindUnqualifiedGName",
}


def load_cost_estimates():
    path = os.path.join(
        os.path.dirname(__file__), "..", "js", "src", "jit", "CacheIROps.yaml"
    )
    costs = {}
    name = None
    for line in open(path):
        m = re.match(r"- name: (\w+)", line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r"\s+cost_estimate: (\d+)", line)
        if m and name:
            costs[name] = int(m.group(1))
    return costs


class OpEffects:
    __slots__ = (
        "transpiled",
        "vm",
        "calls",
        "loads",
        "stores",
        "guard_loads",
        "mir",
    )

    def __init__(self):
        self.transpiled = False
        self.vm = False
        self.calls = False
        self.loads = 0
        self.stores = 0
        self.guard_loads = 0
        self.mir = set()


def classify_ops(mir_pkl):
    import pickle

    with open(mir_pkl, "rb") as f:
        d = pickle.load(f)
    effects = defaultdict(OpEffects)
    for name, n in d["transpiled"].items():
        effects[name].transpiled = True
    for (name, gc), n in d["baseline"].items():
        if gc:
            effects[name].vm = True
    for (op, mir_name, alias, bits), n in d["mir"].items():
        e = effects[op]
        e.mir.add(mir_name)
        if bits & 2:
            e.calls = True
        flags = alias & ~STORE
        if alias & STORE:
            e.stores |= flags
        else:
            e.loads |= flags
            if bits & 4:
                e.guard_loads |= flags
    return effects


class Recipe:
    __slots__ = ("barrier", "reasons", "stores", "guard_loads", "has_guard")

    def __init__(self):
        self.barrier = False
        self.reasons = set()
        self.stores = 0
        self.guard_loads = 0
        self.has_guard = False


def classify_recipe(names, effects, costs, gc_rule):
    r = Recipe()
    for name in names:
        if name in STRUCTURAL_OPS:
            continue
        if name.startswith("Guard"):
            r.has_guard = True
        e = effects.get(name)
        if e is None or not e.transpiled:
            r.barrier = True
            r.reasons.add("opaque")
            continue
        if e.vm:
            r.barrier = True
            r.reasons.add("vm")
        if e.calls:
            r.barrier = True
            r.reasons.add("calls")
        if gc_rule == "cost" and costs.get(name, 0) >= 4:
            r.barrier = True
            r.reasons.add("gc-cost")
        r.stores |= e.stores
        r.guard_loads |= e.guard_loads | (e.loads if name.startswith("Guard") else 0)
    return r


def cut(outcomes, script, recipes, jsop_names, rule):
    """Yields (kind, length) for each run of IC executions in a block instance.

    kind is 'region' for fusible runs and a barrier reason otherwise.
    """
    ops = script.ops
    index_of = {op[0]: i for i, op in enumerate(ops)}
    run = 0
    stores = 0
    prev_index = None
    for pc, rid in outcomes:
        i = index_of[pc]
        between_barrier = None
        if prev_index is not None:
            for j in range(prev_index + 1, i):
                name = jsop_names[ops[j][1]]
                if name not in FRAME_LOCAL_JSOPS:
                    between_barrier = name
                    break
        prev_index = i
        if between_barrier is not None:
            if run:
                yield ("region", run)
            run, stores = 0, 0
            yield ("jsop:" + between_barrier, 0)

        if rid == ictrace.MISS:
            if run:
                yield ("region", run)
            run, stores = 0, 0
            yield ("miss", 1)
            continue
        r = recipes[rid]
        if r.barrier:
            if run:
                yield ("region", run)
            run, stores = 0, 0
            yield ("+".join(sorted(r.reasons)), 1)
            continue
        if stores and r.has_guard:
            blocked = rule == "strict" or (r.guard_loads & stores)
            if blocked:
                yield ("region", run)
                run, stores = 0, 0
        run += 1
        stores |= r.stores
    if run:
        yield ("region", run)


def report(agg_pkl, mir_pkl, gc_rule):
    t, d = ictrace.load(agg_pkl)
    effects = classify_ops(mir_pkl)
    costs = load_cost_estimates()
    jsop_names = [n for _, _, n in t.jsops]
    recipes = []
    for rid in range(len(t.recipe_info)):
        _, names = t.decode_recipe(rid)
        recipes.append(classify_recipe(names, effects, costs, gc_rule))

    total = 0
    for rule in ("strict", "alias"):
        sizes = Counter()
        barriers = Counter()
        jsop_cuts = Counter()
        for (sid, _, outcomes), n in d["instances"].items():
            script = t.scripts[sid]
            for kind, length in cut(outcomes, script, recipes, jsop_names, rule):
                if kind == "region":
                    sizes[length] += n
                elif kind.startswith("jsop:"):
                    jsop_cuts[kind[5:]] += n
                else:
                    barriers[kind] += n * length
        in_regions = sum(k * n for k, n in sizes.items())
        total = in_regions + sum(barriers.values())
        print(f"== rule {rule}, gc {gc_rule}")
        print(f"IC executions {total}")
        print(f"IC executions in regions {in_regions}")
        print(f"IC executions at IC barriers {sum(barriers.values())}")
        for k, n in barriers.most_common():
            print(f"  {k:24s} {n}")
        print("region size k: regions, IC executions, dispatches saved (k-1)")
        for k in sorted(sizes):
            n = sizes[k]
            print(f"  {k:3d}: {n:12d} {k * n:12d} {(k - 1) * n:12d}")
        print(f"dispatches saved {sum((k - 1) * n for k, n in sizes.items())}")
        print("non-IC ops cutting regions:")
        for k, n in jsop_cuts.most_common(15):
            print(f"  {k:24s} {n}")
        print()


def classify(mir_pkl):
    effects = classify_ops(mir_pkl)
    costs = load_cost_estimates()
    for name in sorted(effects):
        e = effects[name]
        tags = []
        if not e.transpiled:
            tags.append("untranspiled")
        if e.vm:
            tags.append("vm")
        if e.calls:
            tags.append("calls")
        if e.stores:
            tags.append(f"store={e.stores:#x}")
        if e.loads:
            tags.append(f"load={e.loads:#x}")
        if e.guard_loads:
            tags.append(f"guardload={e.guard_loads:#x}")
        print(f"{name:40s} cost={costs.get(name, '?')} {' '.join(tags)} "
              f"[{' '.join(sorted(e.mir))}]")


def main():
    args = sys.argv[1:]
    gc_rule = "none"
    if args and args[0].startswith("--gc="):
        gc_rule = args.pop(0)[5:]
    if args[:1] == ["classify"]:
        classify(args[1])
    elif args[:1] == ["report"]:
        report(args[1], args[2], gc_rule)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
