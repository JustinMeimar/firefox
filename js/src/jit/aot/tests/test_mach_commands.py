# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import hashlib
import struct
import sys
from pathlib import Path
from types import SimpleNamespace

import mozunit
import pytest
import yaml
from mach.registrar import Registrar

AOT_DIR = Path(__file__).parents[1]
sys.path[:0] = [str(AOT_DIR), str(AOT_DIR.parent)]
Registrar.register_category("build", "Build", "Build commands")

import mach_commands
from AOTImageFormat import AOTImageFormat
from GenerateAOTImage import describe_schema
from PackAOTImage import pack

FORMAT = AOTImageFormat(
    describe_schema(yaml.safe_load((AOT_DIR.parent / "AOTImageSchema.yaml").read_text()))
)


BUILD_IDENTITY = bytes(range(32))


def write_blob(
    path, kind, slot_hash=0x12345678, code=b"code", variant=0, identity=BUILD_IDENTITY
):
    words = [kind, 0, 0, 0, 0, 0, 0]
    if kind == 0:
        words += [0, variant, 0, 0]
    elif kind == 1:
        words += [0, variant] + [0] * 18
    else:
        words += [0, 0, 0, 0, 0, 0]
    key = struct.pack("<" + "I" * len(words), *words)
    fields = bytes({0: 52, 1: 32, 2: 12}[kind])
    header = FORMAT.blob_header.pack(
        magic=FORMAT.BLOB_FILE_MAGIC,
        version=FORMAT.BLOB_FILE_VERSION,
        reserved=0,
        kind=kind,
        probeHash=0,
        identityHash=hashlib.sha1(key).digest(),
        fieldsSize=len(fields),
        arraysSize=0,
        codeSize=len(code),
        linkSitesSize=0,
        slotTableHash=slot_hash,
        keySize=len(key),
        buildIdentity=identity,
    )
    path.write_bytes(header + key + fields + code)


def corpus(tmp_path):
    path = tmp_path / "corpus"
    path.mkdir()
    write_blob(path / "interp.aotb", 0)
    return path


def identity_file(tmp_path):
    path = tmp_path / "identity.bin"
    path.write_bytes(BUILD_IDENTITY)
    return path


def test_rejects_incompatible_corpus(tmp_path):
    path = corpus(tmp_path)
    write_blob(path / "other.aotb", 1, identity=bytes([42]) * 32)
    with pytest.raises(ValueError, match="mixed build identities"):
        pack(path, [identity_file(tmp_path)], tmp_path / "image", tmp_path / "relocs", FORMAT)


def test_command_construction(tmp_path):
    shell = tmp_path / "obj" / "dist" / "bin" / "js"
    path = tmp_path / "corpus"
    workload = tmp_path / "record.js"
    assert mach_commands._resolve_path("corpus", tmp_path) == path
    assert mach_commands._record_argv(shell, path, workload) == [
        str(shell),
        f"--aot-record={path}",
        "--aot-record-self-hosted",
        "--no-ion",
        "-f",
        str(workload),
    ]

    paths = {
        "pack": AOT_DIR / "PackAOTImage.py",
        "format": tmp_path / "AOTImageFormat.inc",
        "identities": tmp_path / "identities",
        "relocs": tmp_path / "AOTImageRelocs.inc",
        "image": tmp_path / "AOTImage.inc",
    }
    paths["identities"].mkdir()
    identity = identity_file(paths["identities"])
    assert mach_commands._pack_argv(paths, path)[1:] == [
        str(paths["pack"]),
        "--format",
        str(paths["format"]),
        "--build-identity",
        str(identity),
        "--relocs",
        str(paths["relocs"]),
        str(path),
        str(paths["image"]),
    ]

    calls = []
    commands = SimpleNamespace(
        dispatch=lambda *args, **kwargs: calls.append((args, kwargs)) or 0
    )
    context = SimpleNamespace(_mach_context=SimpleNamespace(commands=commands))
    assert mach_commands._relink(context) == 0
    assert calls[0][1] == {"what": ["binaries"]}


def test_test_command_runs_record_pack_relink_and_jit_tests(tmp_path, monkeypatch):
    shell = tmp_path / "dist" / "bin" / "js"
    shell.parent.mkdir(parents=True)
    shell.touch()
    context = SimpleNamespace(topsrcdir=str(tmp_path))
    monkeypatch.setattr(mach_commands, "_paths", lambda _: {"shell": shell})
    monkeypatch.setattr(mach_commands, "_validate_build", lambda *_: None)
    calls = []

    def run_stage(_, label, argv, env=None):
        calls.append((label, argv, env))
        if label == "recording builtins":
            assert Path(argv[-1]).is_dir()
            assert list(Path(argv[-1]).iterdir()) == []

    monkeypatch.setattr(mach_commands, "_run_test_stage", run_stage)
    assert mach_commands.ambermonkey_test(context) == 0
    assert [label for label, _, _ in calls] == [
        "recording builtins",
        "recording jit-tests",
        "packing image",
        "relinking",
        "testing jit-tests",
    ]
    corpus_path = calls[0][1][-1]
    assert calls[0][1][-3:] == ["record", "--corpus", corpus_path]
    assert calls[1][1][-2:] == [
        f"--args=--aot-record={corpus_path} --no-ion",
        str(shell),
    ]
    assert calls[2][1][-3:] == ["pack", "--corpus", corpus_path]
    assert calls[3][1][-1] == "relink"
    assert calls[4][1][-2:] == ["--args=--aot", str(shell)]
    assert not Path(corpus_path).exists()


def test_pack_is_deterministic(tmp_path):
    path = corpus(tmp_path)
    identity = identity_file(tmp_path)
    outputs = [(tmp_path / f"image-{i}", tmp_path / f"relocs-{i}") for i in range(2)]
    for image, relocs in outputs:
        pack(path, [identity], image, relocs, FORMAT)

    assert outputs[0][0].read_bytes() == outputs[1][0].read_bytes()
    assert outputs[0][1].read_bytes() == outputs[1][1].read_bytes()
    image_hash = hashlib.sha256(outputs[0][0].read_bytes()).hexdigest()
    assert image_hash in outputs[0][1].read_text()
    image = mach_commands._read_image(outputs[0][0], FORMAT)
    assert image["hash"] == image_hash
    assert [entry["kind"] for entry in image["entries"]] == [0]


if __name__ == "__main__":
    mozunit.main()
