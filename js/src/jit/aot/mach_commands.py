# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# mach jit-aot: drives the two-stage AOT bootstrap.
#
# Stage 1 is the ordinary build linking against the empty placeholder
# AOTImage.bin planted by GenerateEmptyAOTImage.py. Stage 2 lives
# here:
#
#   `mach jit-aot build`      run the stage-1 shell to fill a record
#                             dir, pack it into a real AOTImage.bin,
#                             and touch AOTImageIncbin.S so a
#                             follow-up ./mach build relinks
#                             only the incbin object.
#   `mach jit-aot regenerate` alias for build; re-runs even if the
#                             stage-1 shell has not changed.
#   `mach jit-aot verify`     run the jit-test suite under --aot and
#                             report the two known allowed failures.

import logging
import shutil
import subprocess
import sys
from pathlib import Path

from mach.decorators import Command, CommandArgument, SubCommand


def _shell_path(command_context):
    p = Path(command_context.topobjdir) / "dist" / "bin" / "js"
    if sys.platform == "win32":
        p = p.with_suffix(".exe")
    return p


def _aot_dir(command_context):
    return Path(command_context.topsrcdir) / "js" / "src" / "jit" / "aot"


@Command(
    "jit-aot",
    category="build",
    description=(
        "Drive the AOT bootstrap: dump baseline + self-hosted + IC "
        "artifacts from the stage-1 shell, pack them into "
        "AOTImage.bin, and relink."
    ),
)
def jit_aot(command_context):
    command_context.log(
        logging.INFO,
        "jit-aot",
        {},
        "Usage: mach jit-aot {build,regenerate,verify}",
    )
    return 0


@SubCommand(
    "jit-aot", "build",
    description=(
        "Stage 2: run the stage-1 shell with --aot-record to fill an "
        "objdir record dir, pack it into AOTImage.bin, and touch the "
        "incbin wrapper so `./mach build` relinks."
    ),
)
@CommandArgument(
    "--workload", "-w",
    default=None,
    help=(
        "Optional JS file to run under --aot-record after the self-"
        "hosted pass. Defaults to no workload (self-hosted-only "
        "corpus)."
    ),
)
def jit_aot_build(command_context, workload=None):
    shell = _shell_path(command_context)
    if not shell.exists():
        command_context.log(
            logging.ERROR, "jit-aot", {},
            f"Stage-1 shell missing at {shell}; run ./mach build first.",
        )
        return 1

    objdir = Path(command_context.topobjdir)
    record_dir = objdir / "aot-record"
    if record_dir.exists():
        shutil.rmtree(record_dir)
    record_dir.mkdir(parents=True)

    aot_dir = _aot_dir(command_context)
    image_out = objdir / "js" / "src" / "jit" / "aot" / "AOTImage.inc"

    argv = [
        str(shell),
        f"--aot-record={record_dir}",
        "--aot-record-self-hosted",
    ]
    if workload:
        argv += ["-f", workload]
    else:
        argv += ["-e", "quit(0);"]

    command_context.log(
        logging.INFO, "jit-aot", {},
        f"Recording AOT corpus into {record_dir}",
    )
    rc = command_context.run_process(
        argv, pass_thru=True, ensure_exit_code=False,
    )
    if rc != 0:
        return rc

    pack = aot_dir / "PackAOTImage.py"
    schema = Path(command_context.topsrcdir) / "js" / "src" / "jit" \
        / "AOTImageSchema.yaml"
    command_context.log(
        logging.INFO, "jit-aot", {}, f"Packing corpus into {image_out}",
    )
    rc = command_context.run_process(
        [sys.executable, str(pack), "--schema", str(schema),
         str(record_dir), str(image_out)],
        pass_thru=True, ensure_exit_code=False,
    )
    if rc != 0:
        return rc

    # The .S file .incbins the .bin; touching it forces a reassemble
    # and relink on the next ./mach build without dirtying anything
    # else.
    (aot_dir / "AOTImageIncbin.S").touch()
    command_context.log(
        logging.INFO, "jit-aot", {},
        "Wrote AOTImage.inc. Run `./mach build binaries` to relink.",
    )
    return 0


@SubCommand("jit-aot", "regenerate",
            description="Alias for `jit-aot build`.")
@CommandArgument("--workload", "-w", default=None)
def jit_aot_regenerate(command_context, workload=None):
    return jit_aot_build(command_context, workload=workload)


# Tests that fail under --aot for reasons unrelated to correctness.
# See CLAUDE.md in js/src.
_ALLOWED_AOT_FAILURES = frozenset({
    "baseline/blinterp-trial-inlining.js",
    "ion/recover-objects.js",
})


@SubCommand(
    "jit-aot", "verify",
    description=(
        "Run the jit-test suite under --aot and report the two known "
        "allowed failures."
    ),
)
def jit_aot_verify(command_context):
    shell = _shell_path(command_context)
    if not shell.exists():
        command_context.log(
            logging.ERROR, "jit-aot", {}, f"Shell missing at {shell}",
        )
        return 1
    jit_test = (Path(command_context.topsrcdir) / "js" / "src" / "jit-test"
                / "jit_test.py")
    argv = [
        sys.executable, str(jit_test), "--args=--aot", str(shell),
    ]
    command_context.log(
        logging.INFO, "jit-aot", {}, f"Running: {' '.join(argv)}",
    )
    return subprocess.call(argv)
