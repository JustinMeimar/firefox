#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Prune a record directory to fit inside a byte budget.

Reads every .aotb file under the input record dir, groups them by
kind, and greedily keeps blobs in ascending code-size order until the
per-kind budget is exhausted. Kept blobs are copied to the output
directory unchanged. Kinds without an explicit budget are copied in
full.

The budgeting policy here is intentionally simple: the paper's
frequency-weighted knapsack (Algorithm 1 in the FrostMonkey draft) is
future work and lives at js/src/jit/SelectAOTCorpus.py in the
aggregate diff; the version here is a standalone size-only fallback.
"""

import argparse
import shutil
import sys
from pathlib import Path

from PackAOTImage import Blob
from AOTImageFormat import AOTImageFormat

def prune(record_dir, out_dir, budgets, format):
    interpreter = next(kind for kind, name in format.kind_names.items()
                       if name == "BaselineInterpreter")
    names = {kind: blob["prefix"] for kind, blob in enumerate(format.blobs)}
    record_dir = Path(record_dir)
    out_dir = Path(out_dir)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    blobs = []
    for p in sorted(record_dir.glob("*.aotb")):
        blob = Blob(p, format)
        blobs.append((blob.kind, blob.code_size, p))

    kept = 0
    dropped = 0
    per_kind_used = {}
    # Interpreter blob is always kept.
    for kind, code_size, p in blobs:
        if kind == interpreter:
            shutil.copy(p, out_dir / p.name)
            kept += 1
    # Everything else: sort by ascending code size within a kind.
    by_kind = {}
    for kind, code_size, p in blobs:
        if kind == interpreter:
            continue
        by_kind.setdefault(kind, []).append((code_size, p))
    for kind, items in by_kind.items():
        items.sort(key=lambda x: x[0])
        limit = budgets.get(names.get(kind, ""), None)
        used = 0
        for code_size, p in items:
            if limit is not None and used + code_size > limit:
                dropped += 1
                continue
            shutil.copy(p, out_dir / p.name)
            used += code_size
            kept += 1
        per_kind_used[names.get(kind, str(kind))] = used

    print(f"kept={kept} dropped={dropped} bytes_per_kind={per_kind_used}")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--format", required=True, help="Build-generated AOTImageFormat.inc")
    p.add_argument("record_dir", help="input directory of .aotb files")
    p.add_argument("out_dir", help="output directory for kept .aotb files")
    p.add_argument(
        "--blfun-budget",
        type=int,
        default=None,
        help="byte budget for baseline-function code; no budget by default",
    )
    p.add_argument(
        "--ic-budget",
        type=int,
        default=None,
        help="byte budget for IC stub code; no budget by default",
    )
    args = p.parse_args(argv)
    prune(
        args.record_dir,
        args.out_dir,
        {
            "blfun": args.blfun_budget,
            "ic": args.ic_budget,
        },
        AOTImageFormat.load(args.format),
    )


if __name__ == "__main__":
    main(sys.argv[1:])
