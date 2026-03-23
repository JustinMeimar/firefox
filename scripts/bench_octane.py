#!/usr/bin/env python3
import argparse
import subprocess
import math
import os
import re

def run_octane(shell, flags, cpu, octane_dir):
    cmd = ["taskset", "-c", cpu, shell] + flags + ["-f", "run.js"]
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=octane_dir)
    benchmarks = {}
    score = None
    for line in result.stdout.splitlines():
        if line.startswith("Score"):
            score = int(line.split()[-1])
        elif m := re.match(r'^(\w[\w\s]*\w)\s*:\s*(\d+)\s*$', line):
            benchmarks[m.group(1)] = int(m.group(2))
    if score is None:
        print(f"  FAILED: {result.stderr.strip()}")
    return score, benchmarks

def stats(values):
    mean = sum(values) / len(values)
    std = math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))
    return mean, std

def print_table(title, labels, rows):
    header = f"{title:<20}"
    for label in labels:
        header += f"  {label:>14}"
    if len(labels) == 2:
        header += f"  {'delta':>10}"
    sep = "-" * len(header)
    print(sep)
    print(header)
    print(sep)
    for name, data in rows:
        row = f"{name:<20}"
        means = {}
        for label in labels:
            vals = data.get(label)
            if vals:
                m, s = stats(vals)
                means[label] = m
                row += f"  {m:>8.0f} ±{s:>4.0f}"
            else:
                row += f"  {'':>14}"
        if len(labels) == 2 and all(l in means for l in labels):
            pct = (means[labels[0]] - means[labels[1]]) / means[labels[1]] * 100
            row += f"  {pct:>+8.1f}%"
        print(row)
    print(sep)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", type=int, default=5)
    args, extra = parser.parse_known_args()

    cpu = os.environ.get("BENCH_CPU", "2")
    shell = os.path.abspath("jsshell")
    octane_dir = os.path.join(os.path.dirname(__file__), "..", "js", "src", "octane")

    configs = [
        ("AOT", ["--aot-bl"] + extra),
        ("JIT", extra),
    ]

    all_results = {}
    all_scores = {}
    for label, flags in configs:
        scores = []
        all_benchmarks = {}
        for i in range(1, args.n + 1):
            print(f"  {label} run {i}/{args.n}...", end=" ", flush=True)
            score, benchmarks = run_octane(shell, flags, cpu, octane_dir)
            print(score if score is not None else "FAILED")
            if score is not None:
                scores.append(score)
                for name, val in benchmarks.items():
                    all_benchmarks.setdefault(name, []).append(val)
        all_results[label] = all_benchmarks
        all_scores[label] = scores

    labels = list(all_results.keys())

    # per-run scores table
    max_runs = max(len(all_scores[l]) for l in labels)
    run_rows = []
    for i in range(max_runs):
        run_data = {}
        for label in labels:
            s = all_scores[label]
            if i < len(s):
                run_data[label] = [s[i]]
        run_rows.append((f"run {i+1}", run_data))
    run_rows.append(("mean", {l: all_scores[l] for l in labels}))
    print_table("Run", labels, run_rows)
    print()

    # per-benchmark table
    bench_names = []
    for benchmarks in all_results.values():
        for name in benchmarks:
            if name not in bench_names:
                bench_names.append(name)

    bench_rows = [(name, {l: all_results[l].get(name, []) for l in labels}) for name in bench_names]
    bench_rows.append(("Score", {l: all_scores[l] for l in labels}))
    print_table("Benchmark", labels, bench_rows)

if __name__ == "__main__":
    main()
