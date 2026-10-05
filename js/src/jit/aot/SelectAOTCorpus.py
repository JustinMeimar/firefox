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
"""

import argparse
import shutil
import sys
from itertools import groupby
from pathlib import Path

from AOTImageFormat import AOTImageFormat
from PackAOTImage import iter_corpus


def prune(record_dir, out_dir, budgets, format):
    record_dir = Path(record_dir)
    out_dir = Path(out_dir)
    source, destination = record_dir.resolve(), out_dir.resolve()
    if (
        source == destination
        or source in destination.parents
        or destination in source.parents
    ):
        raise ValueError("Input and output corpus directories must not overlap")
    blobs = sorted(
        (blob.kind, blob.code_size, blob.source)
        for blob in iter_corpus(record_dir, format)
    )

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    kept = 0
    dropped = 0
    per_kind_used = {}
    for kind, items in groupby(blobs, key=lambda blob: blob[0]):
        name = format.blobs[kind]["prefix"]
        interpreter = format.kind_names[kind] == "BaselineInterpreter"
        limit = None if interpreter else budgets.get(name)
        used = 0
        for _, code_size, path in items:
            if limit is not None and used + code_size > limit:
                dropped += 1
                continue
            shutil.copy(path, out_dir / path.name)
            used += code_size
            kept += 1
        if not interpreter:
            per_kind_used[name] = used

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
