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
from collections import Counter
from pathlib import Path

RE_IC_ATTACH = re.compile(
    r"ic-attach kind=(\w+) code=(\d+)"
    r"(?:\s+aot=\d+)?"
    r"\s+hash=(\d+)"
)

RE_TS = re.compile(r"^ts=\d+\s+")


def parse_profile(path):
    """Return (freq: Counter[(kind,hash) -> count], sizes: dict[(kind,hash) -> bytes])."""
    freq = Counter()
    sizes = {}
    with open(path) as f:
        for line in f:
            line = RE_TS.sub("", line)
            m = RE_IC_ATTACH.match(line)
            if not m:
                continue
            key = (m.group(1), int(m.group(3)))
            freq[key] += 1
            sizes[key] = int(m.group(2))
    return freq, sizes


def select_corpus(profiles, budget):
    """
    Algorithm 1: SelectAOTCorpus.

    For each stub i in the union of all observed stubs:
        n_i  = number of profiles containing i
        v_i  = n_i * sum_j freq_j(i)
        d_i  = v_i / max(size_i, 1)

    Greedily pick the highest-density stub that fits until budget is exhausted.
    """
    all_freqs = []
    sizes = {}
    for path in profiles:
        freq, sz = parse_profile(path)
        all_freqs.append(freq)
        sizes.update(sz)

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

    return selected, density, sizes


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

    selected, density, sizes = select_corpus(args.profiles, args.budget)

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


if __name__ == "__main__":
    main()
