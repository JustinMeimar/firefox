# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import logging
import sys
from pathlib import Path

from mach.decorators import Command, CommandArgument


@Command(
    "aot-refresh",
    category="build",
    description=(
        "Regenerate AOT baseline .bin blobs by running the built shell "
        "with --aot-dump-*. Touches AOTBaselineIncbin.S so a subsequent "
        "`./mach build` reassembles and relinks the aot_baseline library."
    ),
)
def aot_refresh(command_context):
    topsrcdir = Path(command_context.topsrcdir)
    topobjdir = Path(command_context.topobjdir)

    js = topobjdir / "dist" / "bin" / "js"
    if sys.platform == "win32":
        js = js.with_suffix(".exe")
    if not js.exists():
        command_context.log(
            logging.ERROR, "aot-refresh", {},
            f"Shell not found at {js}. Run ./mach build first.",
        )
        return 1

    aot_dir = topsrcdir / "js" / "src" / "jit" / "aot_baseline"
    text_bin = aot_dir / "AOTBaselineText.bin"
    container_bin = aot_dir / "AOTBaselineContainer.bin"
    wrapper_s = aot_dir / "AOTBaselineIncbin.S"

    env = {
        "IONFLAGS": "bl-aot",
        "JS_AOT_TEXT_BIN": str(text_bin),
        "JS_AOT_CONTAINER_BIN": str(container_bin),
    }

    command_context.log(
        logging.INFO, "aot-refresh", {},
        f"Dumping AOT blobs -> {text_bin.name}, {container_bin.name}",
    )
    rc = command_context.run_process(
        [
            str(js),
            "--aot-dump-blinterp",
            "--aot-dump-self-hosted",
            "--aot-dump-ics",
            "-e",
            "quit(0);",
        ],
        pass_thru=True,
        ensure_exit_code=False,
        append_env=env,
    )
    if rc != 0:
        return rc

    # .bin files aren't moz.build-tracked dependencies of the assembler
    # step. Bump the wrapper .S's mtime so mach picks up the change.
    wrapper_s.touch()

    command_context.log(
        logging.INFO, "aot-refresh", {},
        "AOT blobs refreshed. Run './mach build' to relink aot_baseline.",
    )
    return 0


@Command(
    "aot-record",
    category="build",
    description=(
        "Run a workload through the built shell with baseline + IC "
        "recording enabled; dumps the resulting corpus into "
        "js/src/jit/aot_baseline/ and touches AOTBaselineIncbin.S so a "
        "subsequent `./mach build` reassembles."
    ),
)
@CommandArgument(
    "workload",
    nargs="+",
    help=(
        "Shell argv appended to the built js binary, e.g. "
        "`./mach aot-record -- -f benchmark.js` or `./mach aot-record -- "
        "-e 'load(\"perf.js\")'`."
    ),
)
def aot_record(command_context, workload):
    topsrcdir = Path(command_context.topsrcdir)
    topobjdir = Path(command_context.topobjdir)

    js = topobjdir / "dist" / "bin" / "js"
    if sys.platform == "win32":
        js = js.with_suffix(".exe")
    if not js.exists():
        command_context.log(
            logging.ERROR, "aot-record", {},
            f"Shell not found at {js}. Run ./mach build first.",
        )
        return 1

    aot_dir = topsrcdir / "js" / "src" / "jit" / "aot_baseline"
    text_bin = aot_dir / "AOTBaselineText.bin"
    container_bin = aot_dir / "AOTBaselineContainer.bin"
    wrapper_s = aot_dir / "AOTBaselineIncbin.S"

    env = {
        "JS_AOT_TEXT_BIN": str(text_bin),
        "JS_AOT_CONTAINER_BIN": str(container_bin),
    }

    argv = [
        str(js),
        "--aot-record-baseline",
        "--aot-record-ics",
        *workload,
    ]

    command_context.log(
        logging.INFO, "aot-record", {},
        f"Recording AOT corpus from workload: {' '.join(workload)}",
    )
    rc = command_context.run_process(
        argv,
        pass_thru=True,
        ensure_exit_code=False,
        append_env=env,
    )
    if rc != 0:
        command_context.log(
            logging.WARNING, "aot-record", {},
            f"Workload exited with rc={rc}; corpus may be partial.",
        )

    wrapper_s.touch()

    command_context.log(
        logging.INFO, "aot-record", {},
        "Corpus written. Run './mach build' to relink aot_baseline.",
    )
    return rc
