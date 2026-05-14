#!/usr/bin/env python3
"""
PGO-guided IC corpus selector for FrostMonkey.

Reads an IC frequency profile (from JS_AOT_INSTR=ic stderr output) and
an IC dump directory (containing IC-<hash> text files), then selects
the minimal corpus achieving a target coverage threshold.

Usage:
    python SelectAOTCorpus.py \\
        --profile profile.log \\
        --dump-dir /path/to/ic-dump/ \\
        --output-dir js/src/ics/ \\
        --coverage 0.90

The profile is the raw stderr from a JS_AOT_INSTR=ic run. Each line:
    ts=<us> ic-attach kind=<K> code=<bytes> aot=<0|1> hash=<u32>

The dump directory contains IC body text files named IC-<hash>, where
<hash> matches the CacheIRStubKey hash emitted in the profile.
"""
import argparse
import os
import re
import shutil
import json
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
    freq = Counter()
    sizes = {}
    with open(path) as f:
        for line in f:
            line = RE_TS.sub("", line)
            m = RE_IC_ATTACH.match(line)
            if not m:
                continue
            kind = m.group(1)
            code_size = int(m.group(2))
            h = int(m.group(3))
            key = (kind, h)
            freq[key] += 1
            sizes[key] = code_size
    return freq, sizes


def parse_profile_stdin(lines):
    freq = Counter()
    sizes = {}
    for line in lines:
        line = RE_TS.sub("", line)
        m = RE_IC_ATTACH.match(line)
        if not m:
            continue
        kind = m.group(1)
        code_size = int(m.group(2))
        h = int(m.group(3))
        key = (kind, h)
        freq[key] += 1
        sizes[key] = code_size
    return freq, sizes


def select_corpus(freq, sizes, coverage_target, per_kind_floor, size_cap):
    total = sum(freq.values())
    if total == 0:
        return set()

    selected = set()

    if per_kind_floor > 0:
        by_kind = {}
        for (kind, h), count in freq.items():
            by_kind.setdefault(kind, []).append(((kind, h), count))
        for kind, entries in by_kind.items():
            entries.sort(key=lambda x: x[1], reverse=True)
            for key, _ in entries[: per_kind_floor]:
                if size_cap and sizes.get(key, 0) > size_cap:
                    continue
                selected.add(key)

    ranked = freq.most_common()
    running = sum(freq[k] for k in selected)
    for key, count in ranked:
        if running / total >= coverage_target:
            break
        if key in selected:
            continue
        if size_cap and sizes.get(key, 0) > size_cap:
            continue
        selected.add(key)
        running += count

    return selected


def find_dump_files(dump_dir):
    mapping = {}
    for entry in Path(dump_dir).iterdir():
        name = entry.name
        if not name.startswith("IC-"):
            continue
        parts = name.split("-", 2)
        if len(parts) >= 2:
            try:
                h = int(parts[1])
                mapping[h] = entry
            except ValueError:
                pass
    return mapping


def main():
    parser = argparse.ArgumentParser(description="PGO IC corpus selector")
    parser.add_argument("--profile", required=True,
                        help="Path to IC frequency profile (stderr log)")
    parser.add_argument("--dump-dir",
                        help="Directory containing IC-<hash> dump files")
    parser.add_argument("--output-dir", default="js/src/ics",
                        help="Output directory for selected corpus")
    parser.add_argument("--coverage", type=float, default=0.90,
                        help="Target cumulative coverage (0.0-1.0)")
    parser.add_argument("--per-kind-floor", type=int, default=3,
                        help="Minimum stubs per CacheKind")
    parser.add_argument("--size-cap", type=int, default=0,
                        help="Max stub code size in bytes (0 = no cap)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print selection without writing files")
    parser.add_argument("--json", action="store_true",
                        help="Output selection as JSON to stdout")
    args = parser.parse_args()

    freq, sizes = parse_profile(args.profile)
    total = sum(freq.values())
    if total == 0:
        print("No ic-attach events found in profile.", file=sys.stderr)
        sys.exit(1)

    selected = select_corpus(
        freq, sizes,
        coverage_target=args.coverage,
        per_kind_floor=args.per_kind_floor,
        size_cap=args.size_cap or None,
    )

    ranked = freq.most_common()
    sel_count = sum(freq[k] for k in selected)

    if args.json:
        rows = []
        running = 0
        for key, count in ranked:
            running += count
            kind, h = key
            rows.append({
                "kind": kind,
                "hash": h,
                "count": count,
                "bytes": sizes.get(key, 0),
                "cumulative": round(running / total, 4),
                "selected": key in selected,
            })
        json.dump({
            "total_attaches": total,
            "unique_stubs": len(freq),
            "selected_stubs": len(selected),
            "selected_coverage": round(sel_count / total, 4),
            "coverage_target": args.coverage,
            "per_kind_floor": args.per_kind_floor,
            "size_cap": args.size_cap,
            "stubs": rows,
        }, sys.stdout, indent=2)
        sys.exit(0)

    print(f"Profile: {total:,} attach events, {len(freq)} unique stubs")
    print(f"Selected: {len(selected)} stubs covering "
          f"{sel_count/total:.1%} of attaches")
    print()

    running = 0
    for i, (key, count) in enumerate(ranked):
        running += count
        kind, h = key
        mark = "*" if key in selected else " "
        print(f"  {mark} {i+1:4d}. {kind:12s} hash={h:>10d}  "
              f"n={count:5d}  bytes={sizes.get(key,0):4d}  "
              f"cum={running/total:.3f}")
        if i >= 49 and key not in selected:
            remaining = len(freq) - i - 1
            if remaining > 0:
                print(f"  ... {remaining} more stubs in long tail")
            break

    if args.dry_run or not args.dump_dir:
        if not args.dump_dir:
            print("\nNo --dump-dir provided; skipping file selection.",
                  file=sys.stderr)
        return

    dump_files = find_dump_files(args.dump_dir)
    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    for existing in out.glob("IC-*"):
        existing.unlink()

    copied = 0
    missing = 0
    for kind, h in selected:
        if h in dump_files:
            src = dump_files[h]
            shutil.copy2(src, out / src.name)
            copied += 1
        else:
            print(f"  WARN: no dump file for {kind} hash={h}",
                  file=sys.stderr)
            missing += 1

    print(f"\nPopulated {out}: {copied} files copied, {missing} missing")


if __name__ == "__main__":
    main()
