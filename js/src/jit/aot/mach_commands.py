# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import hashlib
import importlib.util
import logging
import os
import shutil
import struct
import sys
from collections import Counter
from pathlib import Path

from mach.decorators import Command, CommandArgument, SubCommand
from mach.registrar import Registrar

PACKER_PATH = Path(__file__).with_name("PackAOTImage.py")
PACKER_SPEC = importlib.util.spec_from_file_location("ambermonkey_packer", PACKER_PATH)
PACKER = importlib.util.module_from_spec(PACKER_SPEC)
PACKER_SPEC.loader.exec_module(PACKER)
Blob = PACKER.Blob


COMMAND = "ambermonkey"
KIND_NAMES = {
    0: "BaselineInterpreter",
    1: "BaselineFunction",
    2: "InlineCacheStub",
}


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
        "identities": image_dir / "build-identities",
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
            f"No configured build found at {paths['objdir']}. "
            "Set MOZCONFIG to the config used for your AOT build, "
            "or configure and build with --enable-aot."
        ) from exc
    if not enabled:
        raise AmberMonkeyError(
            f"The selected build at {paths['objdir']} is not AOT-enabled. "
            "Set MOZCONFIG to the config used for your AOT build, "
            "or add --enable-aot to this build's config and rebuild."
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
    try:
        blobs = [Blob(path) for path in paths]
    except (OSError, ValueError) as exc:
        raise AmberMonkeyError(f"Invalid corpus {corpus}: {exc}") from exc
    return corpus, blobs


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


def _read_image(path):
    path = _resolve_path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise AmberMonkeyError(f"Could not read AOT image {path}: {exc}") from exc
    if len(data) < PACKER.HEADER_SIZE:
        raise AmberMonkeyError(f"AOT image is truncated: {path}")
    header = struct.unpack_from(PACKER.HEADER_FMT, data)
    (
        magic,
        version,
        _reserved,
        count,
        build_identity_offset,
        build_identity_size,
        directory_offset,
        text_offset,
        text_size,
        image_size,
    ) = header
    if magic != PACKER.IMAGE_MAGIC or version != PACKER.IMAGE_VERSION:
        raise AmberMonkeyError(
            f"Unsupported AOT image header in {path}: magic={magic:#x}, version={version}"
        )
    directory_end = directory_offset + count * PACKER.DIR_ENTRY_SIZE
    if (
        image_size != len(data)
        or build_identity_size != PACKER.BUILD_IDENTITY_SIZE
        or build_identity_offset + build_identity_size > len(data)
        or directory_end > text_offset
        or text_offset + text_size > len(data)
    ):
        raise AmberMonkeyError(f"Malformed AOT image layout in {path}")

    entries = []
    for index in range(count):
        entry = struct.unpack_from(
            PACKER.DIR_ENTRY_FMT,
            data,
            directory_offset + index * PACKER.DIR_ENTRY_SIZE,
        )
        kind, probe, identity, code_offset, code_size, data_offset, fields, arrays, key = (
            entry
        )
        if (
            data_offset + key + fields + arrays > text_offset
            or text_offset + code_offset + code_size > len(data)
        ):
            raise AmberMonkeyError(f"Malformed AOT image entry {index} in {path}")
        entries.append({
            "kind": kind,
            "probe": probe,
            "identity": identity.hex(),
            "key": key,
            "fields": fields,
            "arrays": arrays,
            "text_offset": code_offset,
            "text_size": code_size,
        })
    return {
        "path": path,
        "hash": hashlib.sha256(data).hexdigest(),
        "version": version,
        "build_identity": data[
            build_identity_offset : build_identity_offset + build_identity_size
        ].hex(),
        "image_size": image_size,
        "text_offset": text_offset,
        "text_size": text_size,
        "entries": entries,
    }


def _format_image_preview(image, limit):
    totals = Counter()
    for entry in image["entries"]:
        totals[(entry["kind"], "count")] += 1
        for field in ("fields", "arrays", "text_size"):
            totals[(entry["kind"], field)] += entry[field]

    lines = [
        f"Image:       {image['path']}",
        f"Image hash:  {image['hash']}",
        f"Format:      AOTI v{image['version']}",
        f"Build identity: {image['build_identity']}",
        f"Entries:     {len(image['entries'])}",
        f"Image size:  {image['image_size']:,} bytes",
        f"Text:        {image['text_size']:,} bytes at offset {image['text_offset']:,}",
        "",
        "Kind                         Count  Fields (bytes)  Arrays (bytes)    Text (bytes)",
    ]
    for kind in sorted({entry["kind"] for entry in image["entries"]}):
        name = KIND_NAMES.get(kind, f"Unknown({kind})")
        lines.append(
            f"{name:<28} {totals[(kind, 'count')]:>5} "
            f"{totals[(kind, 'fields')]:>14,} {totals[(kind, 'arrays')]:>14,} "
            f"{totals[(kind, 'text_size')]:>15,}"
        )
    if limit:
        lines.extend([
            "",
            f"First {min(limit, len(image['entries']))} entries:",
            "#    Kind                   Probe       Identity      Fields (B)  Arrays (B)  Text offset / size (bytes)",
        ])
        for index, entry in enumerate(image["entries"][:limit]):
            name = KIND_NAMES.get(entry["kind"], f"Unknown({entry['kind']})")
            lines.append(
                f"{index:<4} {name:<22} {entry['probe']:#010x} "
                f"{entry['identity'][:12]} {entry['fields']:>10,} "
                f"{entry['arrays']:>10,} {entry['text_offset']:>11,} / "
                f"{entry['text_size']:,}"
            )
    return "\n".join(lines)


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


def _pack_argv(paths, corpus):
    identities = sorted(paths["identities"].glob("*.bin"))
    if not identities:
        raise AmberMonkeyError("No native build identity found; rebuild stage 1 first.")
    return [
        sys.executable,
        str(paths["pack"]),
        *(arg for path in identities for arg in ("--build-identity", str(path))),
        "--relocs",
        str(paths["relocs"]),
        str(corpus),
        str(paths["image"]),
    ]


def _log_error(command_context, message):
    command_context.log(logging.ERROR, COMMAND, {}, str(message))
    return 1


def _pack(command_context, corpus):
    paths = _paths(command_context)
    _validate_build(command_context, paths)
    corpus, blobs = _load_corpus(corpus)
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
    print("Usage: ./mach ambermonkey <command> [options]\n")
    subcommands = Registrar.command_handlers[COMMAND].subcommand_handlers
    for name, handler in sorted(subcommands.items()):
        print(f"  {name:12} {handler.description}")
    print("\nSet MOZCONFIG to the config used for your AOT build.")
    print("\nRun ./mach ambermonkey <command> --help for options.")
    return 0


@SubCommand(
    "ambermonkey",
    "record",
    description="Record an AOT corpus with the stage-1 shell.",
)
@CommandArgument(
    "--corpus", required=True, help="Directory to receive recorded .aotb files."
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
        corpus, blobs = _load_corpus(corpus)
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
    "show-image",
    description="Preview the contents of a packed AOT image.",
)
@CommandArgument(
    "--image",
    default=None,
    help="Image to inspect (default: the selected objdir's AOTImage.inc).",
)
@CommandArgument(
    "--limit",
    type=int,
    default=12,
    help="Maximum directory entries to show; use 0 for totals only.",
)
def ambermonkey_show_image(command_context, image=None, limit=12):
    try:
        if limit < 0:
            raise AmberMonkeyError("--limit must be zero or greater.")
        path = _resolve_path(image) if image else _paths(command_context)["image"]
        preview = _format_image_preview(_read_image(path), limit)
        command_context.log(logging.INFO, COMMAND, {}, preview)
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
