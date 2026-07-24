#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Reads all .aotb files under a record directory and packs them into a
# single AOTImage.bin. Bit-compatible with the C++ AOTImage reader
# defined in js/src/jit/AOTImage.h; the layout constants below mirror
# the definitions there and must be updated in lockstep.

import argparse
import glob
import hashlib
import os
import struct
import sys

# Wire constants (mirror image::* in js/src/jit/AOTImage.h).
IMAGE_MAGIC = 0x49544F41  # "AOTI"
IMAGE_VERSION = 3
FINGERPRINT_SIZE = 20
ALIGNMENT = 16
TEXT_ALIGNMENT = 4096
# JitCode addresses land in GC cell words that reserve the low 3 bits;
# CodeAlignment on every supported arch is >= 8, so 16 is safe.
CODE_ALIGNMENT = 16

# Header layout: <IHHIIIIIII (36 bytes)
HEADER_FMT = "<IHHIIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 36, HEADER_SIZE

# DirectoryEntry: <II20sIIIII (48 bytes)
DIR_ENTRY_FMT = "<II20sIIIII"
DIR_ENTRY_SIZE = struct.calcsize(DIR_ENTRY_FMT)
assert DIR_ENTRY_SIZE == 48, DIR_ENTRY_SIZE

# Blob file constants (mirror AOTBlobFileHeader).
BLOB_FILE_MAGIC = 0x42544F41  # "AOTB"
BLOB_FILE_VERSION = 1
BLOB_FILE_FMT = "<IHHII20sIII"
BLOB_FILE_HEADER_SIZE = struct.calcsize(BLOB_FILE_FMT)
assert BLOB_FILE_HEADER_SIZE == 48, BLOB_FILE_HEADER_SIZE


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class Blob:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < BLOB_FILE_HEADER_SIZE:
            raise ValueError(f"{path}: truncated blob file")
        (
            magic,
            version,
            _reserved,
            self.kind,
            self.probe_hash,
            self.identity_hash,
            self.fields_size,
            self.arrays_size,
            self.code_size,
        ) = struct.unpack(BLOB_FILE_FMT, data[:BLOB_FILE_HEADER_SIZE])
        if magic != BLOB_FILE_MAGIC:
            raise ValueError(f"{path}: bad magic {magic:#x}")
        if version != BLOB_FILE_VERSION:
            raise ValueError(f"{path}: unsupported version {version}")
        p = BLOB_FILE_HEADER_SIZE
        self.fields = data[p : p + self.fields_size]
        p += self.fields_size
        self.arrays = data[p : p + self.arrays_size]
        p += self.arrays_size
        self.code = data[p : p + self.code_size]
        self.source = path


def compute_fingerprint(schema_path, blobs):
    h = hashlib.sha1()
    h.update(b"aot-image-v")
    h.update(str(IMAGE_VERSION).encode())
    with open(schema_path, "rb") as f:
        h.update(f.read())
    for b in sorted(blobs, key=lambda b: (b.kind, b.identity_hash)):
        h.update(struct.pack("<I", b.kind))
        h.update(b.identity_hash)
    return h.digest()


def pack(record_dir, schema_path, out_path):
    paths = sorted(glob.glob(os.path.join(record_dir, "*.aotb")))
    if not paths:
        print(f"warning: no .aotb files in {record_dir}", file=sys.stderr)
    blobs = [Blob(p) for p in paths]

    fingerprint_offset = HEADER_SIZE
    directory_offset = align_up(fingerprint_offset + FINGERPRINT_SIZE, ALIGNMENT)
    data_start = align_up(directory_offset + DIR_ENTRY_SIZE * len(blobs), ALIGNMENT)

    entries = []
    cursor = data_start
    text_cursor = 0
    for b in blobs:
        text_cursor = align_up(text_cursor, CODE_ALIGNMENT)
        e = {
            "kind": b.kind,
            "probeHash": b.probe_hash,
            "identityHash": b.identity_hash,
            "dataOffset": cursor,
            "fieldsSize": b.fields_size,
            "arraysSize": b.arrays_size,
            "textOffset": text_cursor,
            "textSize": b.code_size,
        }
        cursor = align_up(cursor + b.fields_size + b.arrays_size, ALIGNMENT)
        text_cursor += b.code_size
        entries.append(e)

    data_end = cursor
    text_offset = align_up(data_end, TEXT_ALIGNMENT)
    text_size = align_up(text_cursor, CODE_ALIGNMENT)
    image_size = text_offset + text_size

    buf = bytearray(image_size)

    struct.pack_into(
        HEADER_FMT,
        buf,
        0,
        IMAGE_MAGIC,
        IMAGE_VERSION,
        0,
        len(blobs),
        fingerprint_offset,
        FINGERPRINT_SIZE,
        directory_offset,
        text_offset,
        text_size,
        image_size,
    )

    fp = compute_fingerprint(schema_path, blobs)
    buf[fingerprint_offset : fingerprint_offset + FINGERPRINT_SIZE] = fp

    for i, e in enumerate(entries):
        struct.pack_into(
            DIR_ENTRY_FMT,
            buf,
            directory_offset + i * DIR_ENTRY_SIZE,
            e["kind"],
            e["probeHash"],
            e["identityHash"],
            e["textOffset"],
            e["textSize"],
            e["dataOffset"],
            e["fieldsSize"],
            e["arraysSize"],
        )
        b = blobs[i]
        p = e["dataOffset"]
        buf[p : p + e["fieldsSize"]] = b.fields
        buf[p + e["fieldsSize"] : p + e["fieldsSize"] + e["arraysSize"]] = b.arrays
        t = text_offset + e["textOffset"]
        buf[t : t + e["textSize"]] = b.code

    with open(out_path, "wb") as f:
        f.write(buf)

    print(
        f"packed {len(blobs)} blob(s) into {out_path} "
        f"({image_size} bytes, text {text_size} bytes)"
    )


def main(argv):
    p = argparse.ArgumentParser(description="Pack .aotb files into AOTImage.bin")
    p.add_argument("record_dir", help="Directory of .aotb files")
    p.add_argument("out", help="Output AOTImage.bin path")
    p.add_argument(
        "--schema",
        default=os.path.join(os.path.dirname(__file__), "..", "AOTImageSchema.yaml"),
        help="Schema path for fingerprint input (default: sibling AOTImageSchema.yaml)",
    )
    args = p.parse_args(argv)
    pack(args.record_dir, args.schema, args.out)


if __name__ == "__main__":
    main(sys.argv[1:])
