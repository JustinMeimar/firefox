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
aggregate diff; the version here is a size-only fallback so
`mach jit-aot build` has a deterministic prune step even without
profile input.
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

# Mirror AOTBlobFileHeader (see js/src/jit/AOTImage.h).
BLOB_FILE_MAGIC = 0x42544F41  # "AOTB"
BLOB_FILE_FMT = "<IHHII20sIII"
BLOB_FILE_HEADER_SIZE = struct.calcsize(BLOB_FILE_FMT)

# Mirror AOTBlobKind.
KIND_BASELINE_INTERPRETER = 0
KIND_BASELINE_FUNCTION = 1
KIND_INLINE_CACHE_STUB = 2

KIND_NAME = {
    KIND_BASELINE_INTERPRETER: "interp",
    KIND_BASELINE_FUNCTION: "blfun",
    KIND_INLINE_CACHE_STUB: "ic",
}


def _read_header(path):
    with open(path, "rb") as f:
        buf = f.read(BLOB_FILE_HEADER_SIZE)
    if len(buf) < BLOB_FILE_HEADER_SIZE:
        raise ValueError(f"{path}: truncated header")
    (magic, version, _r, kind, _probe, _id, fields, arrays, code) = (
        struct.unpack(BLOB_FILE_FMT, buf)
    )
    if magic != BLOB_FILE_MAGIC:
        raise ValueError(f"{path}: bad magic {magic:#x}")
    return kind, code


def prune(record_dir, out_dir, budgets):
    record_dir = Path(record_dir)
    out_dir = Path(out_dir)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    blobs = []
    for p in sorted(record_dir.glob("*.aotb")):
        kind, code_size = _read_header(p)
        blobs.append((kind, code_size, p))

    kept = 0
    dropped = 0
    per_kind_used = {}
    # Interpreter blob is always kept.
    for kind, code_size, p in blobs:
        if kind == KIND_BASELINE_INTERPRETER:
            shutil.copy(p, out_dir / p.name)
            kept += 1
    # Everything else: sort by ascending code size within a kind.
    by_kind = {}
    for kind, code_size, p in blobs:
        if kind == KIND_BASELINE_INTERPRETER:
            continue
        by_kind.setdefault(kind, []).append((code_size, p))
    for kind, items in by_kind.items():
        items.sort(key=lambda x: x[0])
        limit = budgets.get(KIND_NAME.get(kind, ""), None)
        used = 0
        for code_size, p in items:
            if limit is not None and used + code_size > limit:
                dropped += 1
                continue
            shutil.copy(p, out_dir / p.name)
            used += code_size
            kept += 1
        per_kind_used[KIND_NAME.get(kind, str(kind))] = used

    print(f"kept={kept} dropped={dropped} bytes_per_kind={per_kind_used}")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("record_dir", help="input directory of .aotb files")
    p.add_argument("out_dir", help="output directory for kept .aotb files")
    p.add_argument(
        "--blfun-budget", type=int, default=None,
        help="byte budget for baseline-function code; no budget by default",
    )
    p.add_argument(
        "--ic-budget", type=int, default=None,
        help="byte budget for IC stub code; no budget by default",
    )
    args = p.parse_args(argv)
    prune(args.record_dir, args.out_dir, {
        "blfun": args.blfun_budget,
        "ic": args.ic_budget,
    })


if __name__ == "__main__":
    main(sys.argv[1:])
