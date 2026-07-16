# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import logging
import os
import sys
from pathlib import Path

from mach.decorators import Command


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
