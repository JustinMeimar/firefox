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
from mach.registrar import Registrar

AOT_DIR = Path(__file__).parents[1]
sys.path.insert(0, str(AOT_DIR))
Registrar.register_category("build", "Build", "Build commands")

import mach_commands
from PackAOTImage import BLOB_FILE_FMT, BLOB_FILE_MAGIC, BLOB_FILE_VERSION, pack


def write_blob(path, kind, slot_hash=0x12345678, fields=b"", code=b""):
    identity = (
        bytes(20) if kind == mach_commands.CONFIGURATION_KIND else bytes([kind]) * 20
    )
    header = struct.pack(
        BLOB_FILE_FMT,
        BLOB_FILE_MAGIC,
        BLOB_FILE_VERSION,
        0,
        kind,
        0,
        identity,
        len(fields),
        0,
        len(code),
        0,
        slot_hash,
    )
    path.write_bytes(header + fields + code)


def corpus(tmp_path):
    path = tmp_path / "corpus"
    path.mkdir()
    write_blob(path / "configuration.aotb", 3, fields=bytes(20))
    write_blob(path / "interp.aotb", 0, fields=b"fields", code=b"code")
    return path


def test_rejects_incompatible_corpus(tmp_path):
    path = corpus(tmp_path)
    write_blob(path / "other.aotb", 1, slot_hash=0x87654321)
    with pytest.raises(mach_commands.AmberMonkeyError, match="fingerprint"):
        mach_commands._load_corpus(path)


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
        "schema": tmp_path / "schema.yaml",
        "relocs": tmp_path / "AOTImageRelocs.inc",
        "image": tmp_path / "AOTImage.inc",
    }
    assert mach_commands._pack_argv(paths, path)[1:] == [
        str(paths["pack"]),
        "--schema",
        str(paths["schema"]),
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


def test_pack_is_deterministic(tmp_path):
    path = corpus(tmp_path)
    schema = tmp_path / "schema.yaml"
    schema.write_text("schema\n")
    outputs = [(tmp_path / f"image-{i}", tmp_path / f"relocs-{i}") for i in range(2)]
    for image, relocs in outputs:
        pack(path, schema, image, relocs)

    assert outputs[0][0].read_bytes() == outputs[1][0].read_bytes()
    assert outputs[0][1].read_bytes() == outputs[1][1].read_bytes()
    image_hash = hashlib.sha256(outputs[0][0].read_bytes()).hexdigest()
    assert image_hash in outputs[0][1].read_text()
    image = mach_commands._read_image(outputs[0][0])
    assert image["hash"] == image_hash
    assert [entry["kind"] for entry in image["entries"]] == [3, 0]


if __name__ == "__main__":
    mozunit.main()
