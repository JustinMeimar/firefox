# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import hashlib
import importlib.util
import logging
import os
import shutil
import sys
import tempfile
from pathlib import Path

from mach.decorators import Command, CommandArgument, SubCommand

PACKER_PATH = Path(__file__).with_name("PackAOTImage.py")
PACKER_SPEC = importlib.util.spec_from_file_location("ambermonkey_packer", PACKER_PATH)
PACKER = importlib.util.module_from_spec(PACKER_SPEC)
PACKER_SPEC.loader.exec_module(PACKER)
Blob = PACKER.Blob


COMMAND = "ambermonkey"
CONFIGURATION_KIND = 3
CONFIGURATION_FIELDS_SIZE = 20


class AmberMonkeyError(Exception):
    pass


def _resolve_path(path, cwd=None):
    path = Path(path).expanduser()
    if not path.is_absolute():
        path = Path(cwd or Path.cwd()) / path
    return path.resolve()


def _executable_path(objdir, name):
    path = Path(objdir) / "dist" / "bin" / name
    if sys.platform == "win32":
        path = path.with_suffix(".exe")
    return path


def _paths(command_context):
    topsrcdir = Path(command_context.topsrcdir)
    objdir = Path(command_context.topobjdir)
    aot_srcdir = topsrcdir / "js" / "src" / "jit" / "aot"
    image_dir = objdir / "js" / "src" / "jit" / "aot"
    return {
        "objdir": objdir,
        "aot_srcdir": aot_srcdir,
        "schema": topsrcdir / "js" / "src" / "jit" / "AOTImageSchema.yaml",
        "pack": aot_srcdir / "PackAOTImage.py",
        "image": image_dir / "AOTImage.inc",
        "relocs": image_dir / "AOTImageRelocs.inc",
        "shell": _executable_path(objdir, "js"),
        "firefox": _executable_path(objdir, "firefox"),
    }


def _expected_executables(command_context, paths):
    if command_context.substs.get("MOZ_BUILD_APP") == "browser":
        return [paths["firefox"]]
    return [paths["shell"]]


def _validate_build(command_context, paths):
    try:
        enabled = command_context.substs.get("ENABLE_JS_AOT")
    except Exception as exc:
        raise AmberMonkeyError(
            f"No configured build found at {paths['objdir']}; run ./mach configure "
            "with --enable-aot and build the empty-image stage 1."
        ) from exc
    if not enabled:
        raise AmberMonkeyError(
            f"The build at {paths['objdir']} is not AOT-enabled; add "
            "--enable-aot to its configure options and rebuild."
        )
    missing = [
        path
        for path in _expected_executables(command_context, paths)
        if not path.is_file()
    ]
    if missing:
        formatted = "\n  ".join(str(path) for path in missing)
        raise AmberMonkeyError(
            f"Stage-1 executable(s) missing:\n  {formatted}\n"
            "Build the empty-image stage 1 before packing a corpus."
        )


def _load_corpus(corpus):
    corpus = _resolve_path(corpus)
    if not corpus.is_dir():
        raise AmberMonkeyError(f"Corpus directory does not exist: {corpus}")
    paths = sorted(corpus.glob("*.aotb"), key=lambda path: path.name)
    if not paths:
        raise AmberMonkeyError(f"Corpus contains no .aotb files: {corpus}")
    try:
        blobs = [Blob(path) for path in paths]
    except (OSError, ValueError) as exc:
        raise AmberMonkeyError(f"Invalid corpus {corpus}: {exc}") from exc
    configurations = [blob for blob in blobs if blob.kind == CONFIGURATION_KIND]
    configuration_path = corpus / "configuration.aotb"
    if not configuration_path.is_file():
        raise AmberMonkeyError(
            f"Corpus is missing {configuration_path.name}; record it with "
            "`./mach ambermonkey record`."
        )
    if len(configurations) != 1 or Path(configurations[0].source) != configuration_path:
        raise AmberMonkeyError(
            "Corpus must contain exactly one Configuration blob named "
            "configuration.aotb."
        )
    configuration = configurations[0]
    if (
        configuration.identity_hash != bytes(20)
        or configuration.fields_size != CONFIGURATION_FIELDS_SIZE
        or configuration.arrays_size
        or configuration.code_size
        or configuration.link_sites
    ):
        raise AmberMonkeyError(
            f"Malformed configuration metadata in {configuration_path}; "
            "record the corpus again."
        )
    slot_hash = configuration.slot_table_hash
    incompatible = [blob.source for blob in blobs if blob.slot_table_hash != slot_hash]
    if incompatible:
        raise AmberMonkeyError(
            f"{incompatible[0]} has a different AOT link-table fingerprint; "
            "the corpus combines artifacts from incompatible builds."
        )
    return corpus, blobs, configuration


def _corpus_hash(corpus, paths):
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda path: path.name):
        data = path.read_bytes()
        name = path.relative_to(corpus).as_posix().encode()
        digest.update(len(name).to_bytes(8, "little"))
        digest.update(name)
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return digest.hexdigest()


def _clean_environment():
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("JIT_OPTION_") and key != "IONFLAGS"
    }


def _record_argv(shell, corpus, workload=None):
    argv = [
        str(shell),
        f"--aot-record={corpus}",
        "--aot-record-self-hosted",
        "--no-ion",
    ]
    if workload:
        argv.extend(["-f", str(workload)])
    else:
        argv.extend(["-e", "quit(0);"])
    return argv


def _configuration_argv(shell, corpus):
    return [str(shell), f"--aot-record={corpus}", "--no-ion", "-e", "quit(0);"]


def _pack_argv(paths, corpus):
    return [
        sys.executable,
        str(paths["pack"]),
        "--schema",
        str(paths["schema"]),
        "--relocs",
        str(paths["relocs"]),
        str(corpus),
        str(paths["image"]),
    ]


def _validate_configuration(command_context, paths, recorded):
    if not paths["shell"].is_file():
        return
    validation_root = paths["objdir"] / "ambermonkey"
    validation_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="validate-", dir=validation_root) as tmp:
        rc = command_context.run_process(
            _configuration_argv(paths["shell"], Path(tmp)),
            explicit_env=_clean_environment(),
            pass_thru=True,
            ensure_exit_code=False,
        )
        if rc:
            raise AmberMonkeyError(
                f"Could not read the stage-1 AOT configuration (js exited {rc})."
            )
        try:
            current = Blob(Path(tmp) / "configuration.aotb")
        except (OSError, ValueError) as exc:
            raise AmberMonkeyError(
                "The stage-1 shell did not produce valid AOT configuration metadata."
            ) from exc
    if current.slot_table_hash != recorded.slot_table_hash:
        raise AmberMonkeyError(
            "Corpus AOT link-table fingerprint is stale or belongs to another "
            f"build ({recorded.slot_table_hash:#010x} != "
            f"{current.slot_table_hash:#010x}); record the corpus again."
        )
    if current.fields != recorded.fields or current.arrays != recorded.arrays:
        raise AmberMonkeyError(
            "configuration.aotb is incompatible with the configured stage-1 "
            "shell; record the corpus again with this build."
        )


def _log_error(command_context, message):
    command_context.log(logging.ERROR, COMMAND, {}, str(message))
    return 1


def _pack(command_context, corpus):
    paths = _paths(command_context)
    _validate_build(command_context, paths)
    corpus, blobs, configuration = _load_corpus(corpus)
    _validate_configuration(command_context, paths, configuration)
    paths["image"].parent.mkdir(parents=True, exist_ok=True)
    rc = command_context.run_process(
        _pack_argv(paths, corpus), pass_thru=True, ensure_exit_code=False
    )
    if rc:
        raise AmberMonkeyError(f"PackAOTImage.py exited with status {rc}.")
    return {
        "paths": paths,
        "corpus": corpus,
        "corpus_hash": _corpus_hash(corpus, [Path(blob.source) for blob in blobs]),
        "image_hash": hashlib.sha256(paths["image"].read_bytes()).hexdigest(),
    }


def _relink(command_context):
    return command_context._mach_context.commands.dispatch(
        "build", command_context._mach_context, what=["binaries"]
    )


def _print_summary(command_context, result, executables=None):
    paths = result["paths"]
    lines = [
        f"Corpus:      {result['corpus']}",
        f"Corpus hash: {result['corpus_hash']}",
        f"Image hash:  {result['image_hash']}",
        f"Image:       {paths['image']}",
        f"Objdir:      {paths['objdir']}",
    ]
    if result.get("output_dir"):
        lines.append(f"Bundle:      {result['output_dir']}")
    for executable in executables or []:
        lines.append(f"Executable:  {executable}")
    command_context.log(logging.INFO, COMMAND, {}, "\n".join(lines))


def _bundle(paths, executables, output_dir):
    source = paths["objdir"] / "dist" / "bin"
    output_dir = _resolve_path(output_dir)
    if output_dir.exists():
        raise AmberMonkeyError(
            f"Bundle output already exists: {output_dir}; choose a new path."
        )
    if source == output_dir or source in output_dir.parents:
        raise AmberMonkeyError(
            f"Bundle output must not be inside the source directory {source}."
        )
    shutil.copytree(source, output_dir)
    return output_dir, [
        output_dir / executable.relative_to(source) for executable in executables
    ]


@Command(
    "ambermonkey",
    category="build",
    description="Record, pack, relink, and verify AmberMonkey AOT images.",
)
def ambermonkey(command_context):
    command_context.log(
        logging.INFO,
        COMMAND,
        {},
        "Usage: mach ambermonkey {record,pack,relink,build-image,verify}",
    )
    return 0


@SubCommand(
    "ambermonkey",
    "record",
    description="Record an AOT corpus with the stage-1 shell.",
)
@CommandArgument(
    "--corpus", required=True, help="Empty directory to receive recorded .aotb files."
)
@CommandArgument(
    "--workload", "-w", default=None, help="Optional JS workload to record."
)
def ambermonkey_record(command_context, corpus, workload=None):
    try:
        paths = _paths(command_context)
        _validate_build(command_context, paths)
        if not paths["shell"].is_file():
            raise AmberMonkeyError(
                f"Recording requires a stage-1 JS shell at {paths['shell']}; "
                "select an AOT-enabled JS-shell objdir."
            )
        corpus = _resolve_path(corpus)
        if corpus.exists() and any(corpus.iterdir()):
            raise AmberMonkeyError(
                f"Refusing to record into non-empty directory {corpus}; choose a new corpus path."
            )
        corpus.mkdir(parents=True, exist_ok=True)
        workload = _resolve_path(workload) if workload else None
        if workload and not workload.is_file():
            raise AmberMonkeyError(f"Workload does not exist: {workload}")
        rc = command_context.run_process(
            _record_argv(paths["shell"], corpus, workload),
            explicit_env=_clean_environment(),
            pass_thru=True,
            ensure_exit_code=False,
        )
        if rc:
            return rc
        corpus, blobs, _ = _load_corpus(corpus)
        command_context.log(
            logging.INFO,
            COMMAND,
            {},
            f"Corpus:      {corpus}\n"
            f"Corpus hash: {_corpus_hash(corpus, [Path(blob.source) for blob in blobs])}",
        )
        return 0
    except (AmberMonkeyError, OSError) as exc:
        return _log_error(command_context, exc)


@SubCommand(
    "ambermonkey",
    "pack",
    description="Validate and deterministically pack an existing corpus.",
)
@CommandArgument(
    "--corpus", required=True, help="Directory containing recorded .aotb files."
)
def ambermonkey_pack(command_context, corpus):
    try:
        result = _pack(command_context, corpus)
        _print_summary(command_context, result)
        return 0
    except (AmberMonkeyError, OSError) as exc:
        return _log_error(command_context, exc)


@SubCommand(
    "ambermonkey",
    "relink",
    description="Relink binaries with the currently packed objdir image.",
)
def ambermonkey_relink(command_context):
    try:
        paths = _paths(command_context)
        _validate_build(command_context, paths)
        if not paths["image"].is_file() or not paths["relocs"].is_file():
            raise AmberMonkeyError(
                "No packed objdir image found; run `./mach ambermonkey pack --corpus PATH` first."
            )
        return _relink(command_context)
    except (AmberMonkeyError, OSError) as exc:
        return _log_error(command_context, exc)


@SubCommand(
    "ambermonkey",
    "build-image",
    description="Pack a corpus and minimally relink AOT binaries.",
)
@CommandArgument(
    "--corpus", required=True, help="Directory containing recorded .aotb files."
)
@CommandArgument(
    "--output-dir",
    default=None,
    help="Optional new directory to receive a runnable copy of dist/bin.",
)
def ambermonkey_build_image(command_context, corpus, output_dir=None):
    try:
        if output_dir and _resolve_path(output_dir).exists():
            raise AmberMonkeyError(
                f"Bundle output already exists: {_resolve_path(output_dir)}; choose a new path."
            )
        result = _pack(command_context, corpus)
        rc = _relink(command_context)
        if rc:
            return rc
        executables = _expected_executables(command_context, result["paths"])
        missing = [path for path in executables if not path.is_file()]
        if missing:
            raise AmberMonkeyError(
                f"Relink succeeded but expected executable is missing: {missing[0]}"
            )
        if output_dir:
            result["output_dir"], executables = _bundle(
                result["paths"], executables, output_dir
            )
        _print_summary(command_context, result, executables)
        return 0
    except (AmberMonkeyError, OSError) as exc:
        return _log_error(command_context, exc)


@SubCommand(
    "ambermonkey", "verify", description="Run jit-tests against the linked AOT image."
)
def ambermonkey_verify(command_context):
    try:
        paths = _paths(command_context)
        _validate_build(command_context, paths)
        if not paths["shell"].is_file():
            raise AmberMonkeyError(
                f"Verification requires a JS shell at {paths['shell']}; "
                "select an AOT-enabled JS-shell objdir."
            )
    except AmberMonkeyError as exc:
        return _log_error(command_context, exc)
    jit_test = (
        Path(command_context.topsrcdir) / "js" / "src" / "jit-test" / "jit_test.py"
    )
    return command_context.run_process(
        [sys.executable, str(jit_test), "--args=--aot", str(paths["shell"])],
        pass_thru=True,
        ensure_exit_code=False,
    )
