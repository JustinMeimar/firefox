#!/usr/bin/env python3
"""
PGO-guided IC corpus selector for FrostMonkey.

Implements Algorithm 1 (SelectAOTCorpus) from the paper: given k IC
frequency profiles and a byte budget B, select the corpus S that
maximises workload-weighted call frequency per code byte.

Usage:
    python SelectAOTCorpus.py \
        --profiles profile1.log profile2.log ... \
        --dump-dir /path/to/ic-dump/ \
        --output-dir js/src/ics/ \
        --budget 8192
"""
import argparse
import re
import shutil
import sys
from collections import Counter, defaultdict
from pathlib import Path

RE_IC_ATTACH = re.compile(
    r"ic-attach kind=(\w+) code=(\d+)"
    r"(?:\s+aot=\d+)?"
    r"\s+hash=(\d+)"
    r"\s+script=(\d+)"
    r"\s+pc=(\d+)"
)

RE_TS = re.compile(r"^ts=\d+\s+")


def parse_profile(path):
    """Parse one profile log.

    Returns:
      freq:  Counter[(kind, hash) -> count]              -- corpus selection
      sizes: dict[(kind, hash) -> code_bytes]            -- knapsack weight
      sites: dict[(kind, hash) -> set[(script, pc)]]     -- eager-attach hints
             (script=0 sites are dropped: eval/dyngen/inlined ICScripts have
              no stable identity across runs)
    """
    freq = Counter()
    sizes = {}
    sites = {}
    with open(path) as f:
        for line in f:
            line = RE_TS.sub("", line)
            m = RE_IC_ATTACH.match(line)
            if not m:
                continue
            kind, code_bytes, h, script, pc = m.groups()
            key = (kind, int(h))
            freq[key] += 1
            sizes[key] = int(code_bytes)
            script_i = int(script)
            if script_i != 0:
                sites.setdefault(key, set()).add((script_i, int(pc)))
    return freq, sizes, sites


def select_corpus(profiles, budget):
    """
    Algorithm 1: SelectAOTCorpus.

    For each stub i in the union of all observed stubs:
        n_i  = number of profiles containing i
        v_i  = n_i * sum_j freq_j(i)
        d_i  = v_i / max(size_i, 1)

    Greedily pick the highest-density stub that fits until budget is exhausted.

    Also returns the union of observed (script, pc) sites per stub, for
    eager-attach hint emission.
    """
    all_freqs = []
    sizes = {}
    all_sites = defaultdict(set)
    for path in profiles:
        freq, sz, sites = parse_profile(path)
        all_freqs.append(freq)
        sizes.update(sz)
        for key, s in sites.items():
            all_sites[key] |= s

    stubs = set()
    for freq in all_freqs:
        stubs.update(freq.keys())

    density = {}
    for i in stubs:
        n_i = sum(1 for freq in all_freqs if i in freq)
        v_i = n_i * sum(freq[i] for freq in all_freqs)
        density[i] = v_i / max(sizes.get(i, 1), 1)

    selected = set()
    b = 0
    while b < budget:
        best = None
        best_d = -1
        for i in stubs - selected:
            s = sizes.get(i, 0)
            if b + s > budget:
                continue
            if density[i] > best_d:
                best = i
                best_d = density[i]
        if best is None:
            break
        selected.add(best)
        b += sizes.get(best, 0)

    return selected, density, sizes, all_sites


def write_hints_tbl(path, selected, sites):
    """Emit AOTHints.tbl as readable text, one hint per line:

        scriptKey pcOffset stubHash

    sorted by (scriptKey, pcOffset). Only sites of selected corpus stubs
    are emitted. GenerateCacheIRFiles.py resolves stubHash to a corpus
    index at build time and filters to zero-stub-data stubs.
    """
    hints = []
    for (kind, stub_hash) in selected:
        for (script_key, pc) in sites.get((kind, stub_hash), ()):
            hints.append((script_key, pc, stub_hash))
    hints.sort()

    with open(path, "w") as f:
        f.write("# AOT eager-attach IC hints, written by SelectAOTCorpus.py.\n")
        f.write("# scriptKey pcOffset stubHash\n")
        for script_key, pc, stub_hash in hints:
            f.write(f"{script_key} {pc} {stub_hash}\n")

    return len(hints), len({h[0] for h in hints})


def find_dump_files(dump_dir):
    mapping = {}
    for entry in Path(dump_dir).iterdir():
        if not entry.name.startswith("IC-"):
            continue
        parts = entry.name.split("-", 2)
        if len(parts) >= 2:
            try:
                mapping[int(parts[1])] = entry
            except ValueError:
                pass
    return mapping


def main():
    parser = argparse.ArgumentParser(description="PGO IC corpus selector (Algorithm 1)")
    parser.add_argument("--profiles", nargs="+", required=True,
                        help="IC frequency profile logs (one per workload)")
    parser.add_argument("--budget", type=int, required=True,
                        help="Byte budget B for selected corpus")
    parser.add_argument("--dump-dir",
                        help="Directory containing IC-<hash> dump files")
    parser.add_argument("--output-dir", default="js/src/ics",
                        help="Output directory for selected corpus")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print selection without writing files")
    args = parser.parse_args()

    selected, density, sizes, sites = select_corpus(args.profiles, args.budget)

    if not selected:
        print("No stubs selected.", file=sys.stderr)
        sys.exit(1)

    total_bytes = sum(sizes.get(k, 0) for k in selected)
    ranked = sorted(selected, key=lambda k: density[k], reverse=True)

    print(f"Selected {len(selected)} stubs, {total_bytes} / {args.budget} bytes")
    for i, key in enumerate(ranked):
        kind, h = key
        print(f"  {i+1:4d}. {kind:12s} hash={h:>10d}  "
              f"bytes={sizes.get(key,0):4d}  density={density[key]:.1f}")

    if args.dry_run or not args.dump_dir:
        return

    dump_files = find_dump_files(args.dump_dir)
    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    for existing in out.glob("IC-*"):
        existing.unlink()

    copied, missing = 0, 0
    for kind, h in selected:
        if h in dump_files:
            shutil.copy2(dump_files[h], out / dump_files[h].name)
            copied += 1
        else:
            print(f"  WARN: no dump file for {kind} hash={h}", file=sys.stderr)
            missing += 1

    print(f"\nPopulated {out}: {copied} files copied, {missing} missing")

    hints_path = out / "AOTHints.tbl"
    n_hints, n_scripts = write_hints_tbl(hints_path, selected, sites)
    print(f"Wrote {hints_path}: {n_hints} hints across {n_scripts} scripts")


if __name__ == "__main__":
    main()
