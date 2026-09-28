#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Combines recorded artifacts into one image. Its layout constants must remain
# synchronized with the runtime reader.

import argparse
import glob
import hashlib
import os
import struct
import sys

# Constants shared with the runtime image format.
IMAGE_MAGIC = 0x49544F41  # "AOTI"
IMAGE_VERSION = 4
BUILD_IDENTITY_SIZE = 32
ALIGNMENT = 16
TEXT_ALIGNMENT = 4096
# JIT code addresses reserve three low bits in GC cell words. All supported
# targets use at least eight byte alignment, so sixteen byte alignment is
# sufficient.
CODE_ALIGNMENT = 16

# The image header occupies 36 bytes.
HEADER_FMT = "<IHHIIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 36, HEADER_SIZE

# Each directory entry occupies 52 bytes.
DIR_ENTRY_FMT = "<II20sIIIIII"
DIR_ENTRY_SIZE = struct.calcsize(DIR_ENTRY_FMT)
assert DIR_ENTRY_SIZE == 52, DIR_ENTRY_SIZE

# Constants shared with the intermediate artifact header.
BLOB_FILE_MAGIC = 0x42544F41  # "AOTB"
BLOB_FILE_VERSION = 3
BLOB_FILE_FMT = "<IHHII20sIIIIII32s"
BLOB_FILE_HEADER_SIZE = struct.calcsize(BLOB_FILE_FMT)
assert BLOB_FILE_HEADER_SIZE == 92, BLOB_FILE_HEADER_SIZE

LINK_SITE_FMT = "<II"
LINK_SITE_SIZE = struct.calcsize(LINK_SITE_FMT)
assert LINK_SITE_SIZE == 8, LINK_SITE_SIZE

# Every link site is a four byte displacement the static linker computes.
LINK_SITE_WIDTH = 4


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class Blob:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < 8:
            raise ValueError(f"{path}: truncated blob file")
        magic, version = struct.unpack_from("<IH", data)
        if magic != BLOB_FILE_MAGIC:
            raise ValueError(f"{path}: bad magic {magic:#x}")
        if version != BLOB_FILE_VERSION:
            raise ValueError(f"{path}: unsupported version {version}; re-record the corpus")
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
            link_sites_size,
            self.slot_table_hash,
            self.key_size,
            self.build_identity,
        ) = struct.unpack(BLOB_FILE_FMT, data[:BLOB_FILE_HEADER_SIZE])
        if link_sites_size % LINK_SITE_SIZE:
            raise ValueError(
                f"{path}: link site table is not a whole number of entries"
            )
        expected_size = (
            BLOB_FILE_HEADER_SIZE
            + self.key_size
            + self.fields_size
            + self.arrays_size
            + self.code_size
            + link_sites_size
        )
        if len(data) != expected_size:
            raise ValueError(
                f"{path}: file size is {len(data)} bytes, expected {expected_size}"
            )
        p = BLOB_FILE_HEADER_SIZE
        self.key = data[p : p + self.key_size]
        p += self.key_size
        self.fields = data[p : p + self.fields_size]
        p += self.fields_size
        self.arrays = data[p : p + self.arrays_size]
        p += self.arrays_size
        self.code = data[p : p + self.code_size]
        p += self.code_size
        self.link_sites = [
            struct.unpack_from(LINK_SITE_FMT, data, p + i * LINK_SITE_SIZE)
            for i in range(link_sites_size // LINK_SITE_SIZE)
        ]
        self.source = path
        if _reserved or self.kind not in (0, 1, 2):
            raise ValueError(f"{path}: unsupported artifact kind or flags")
        if self.key_size < 4 or self.key_size % 4:
            raise ValueError(f"{path}: malformed compilation key")
        if struct.unpack_from("<I", self.key)[0] != self.kind:
            raise ValueError(f"{path}: key kind differs from artifact kind")
        if hashlib.sha1(self.key).digest() != self.identity_hash:
            raise ValueError(f"{path}: compilation key digest mismatch")
        if not any(self.build_identity):
            raise ValueError(f"{path}: missing build identity")
        fields_size, array_widths = {
            0: (52, (4, 4, 4, 8)),
            1: (32, (8, 8, 8, 8)),
            2: (12, (1, 1)),
        }[self.kind]
        if self.fields_size != fields_size:
            raise ValueError(f"{path}: wrong metadata size")
        counts = struct.unpack_from(
            "<" + "I" * len(array_widths), self.fields,
            fields_size - 4 * len(array_widths)
        )
        if sum(n * width for n, width in zip(counts, array_widths)) != self.arrays_size:
            raise ValueError(f"{path}: malformed metadata arrays")
        cursor = 0

        def word():
            nonlocal cursor
            if cursor + 4 > len(self.key):
                raise ValueError(f"{path}: truncated compilation key")
            value = struct.unpack_from("<I", self.key, cursor)[0]
            cursor += 4
            return value

        def boolean():
            if word() > 1:
                raise ValueError(f"{path}: invalid context boolean")

        def byte_string():
            nonlocal cursor
            length = word()
            end = cursor + length
            padded = align_up(end, 4)
            if padded > len(self.key) or any(self.key[end:padded]):
                raise ValueError(f"{path}: invalid key byte string")
            result = self.key[cursor:end]
            cursor = padded
            return result

        word()  # kind, checked above
        word()  # CPU features
        for _ in range(5):
            boolean()
        if self.kind == 2:
            boolean()
            boolean()
            if word() != self.fields[0] or word() != self.fields[2]:
                raise ValueError(f"{path}: IC inputs differ from metadata")
            if byte_string() != self.arrays[:counts[0]] or byte_string() != self.arrays[counts[0]:]:
                raise ValueError(f"{path}: IC inputs differ from metadata")
        else:
            boolean()
            word()
            if self.kind == 0:
                boolean()
                boolean()
                word()
                word()
            else:
                word()
                word()
                boolean()
                word()
                boolean()
                boolean()
                for _ in range(7):
                    word()
                for _ in range(4):
                    boolean()
                count = word()
                if count > (len(self.key) - cursor) // 4:
                    raise ValueError(f"{path}: truncated GC-thing kinds")
                cursor += count * 4
                byte_string()
        if cursor != len(self.key):
            raise ValueError(f"{path}: trailing compilation key bytes")
        previous = -LINK_SITE_WIDTH
        for offset, slot in sorted(self.link_sites):
            if offset < previous + LINK_SITE_WIDTH or offset + LINK_SITE_WIDTH > self.code_size:
                raise ValueError(f"{path}: invalid or overlapping link site")
            if any(self.code[offset:offset + LINK_SITE_WIDTH]):
                raise ValueError(f"{path}: nonzero link displacement")
            previous = offset


def emit_relocs(path, buf, sites, slot_table_hash):
    """Writes the interleaved chunk and site list the image shim expands.

    A site replaces four image bytes with a displacement the static linker
    computes, so the shim embeds the image as the runs of bytes between sites
    rather than as one blob.
    """
    lines = [
        "// Generated by PackAOTImage.py. Do not edit.",
        f"// Image SHA-256: {hashlib.sha256(buf).hexdigest()}",
        f"AOT_IMAGE_SLOT_TABLE_HASH({slot_table_hash:#010x}u)",
    ]
    cursor = 0
    for offset, slot in sites:
        if offset < cursor:
            raise ValueError(f"link site at {offset} overlaps the preceding site")
        if offset + LINK_SITE_WIDTH > len(buf):
            raise ValueError(f"link site at {offset} runs past the image")
        if any(buf[offset : offset + LINK_SITE_WIDTH]):
            raise ValueError(f"link site at {offset} does not land on a zero field")
        if offset > cursor:
            lines.append(f"AOT_IMAGE_CHUNK({cursor}, {offset - cursor})")
        lines.append(f"AOT_IMAGE_SITE({slot})")
        cursor = offset + LINK_SITE_WIDTH
    if cursor < len(buf):
        lines.append(f"AOT_IMAGE_CHUNK({cursor}, {len(buf) - cursor})")

    with open(path, "wb") as f:
        f.write("\n".join(lines).encode() + b"\n")


def pack(record_dir, build_identities, out_path, relocs_path):
    paths = sorted(glob.glob(os.path.join(record_dir, "*.aotb")))
    if not paths:
        print(f"warning: no .aotb files in {record_dir}", file=sys.stderr)
    blobs = [Blob(p) for p in paths]

    identities = []
    for path in build_identities:
        with open(path, "rb") as stream:
            identities.append(stream.read())
    if not identities or any(len(value) != BUILD_IDENTITY_SIZE for value in identities):
        raise ValueError("A destination build identity is required; build stage 1 first")
    build_identity = blobs[0].build_identity if blobs else identities[0]
    if build_identity not in identities:
        raise ValueError("Corpus belongs to another build; re-record with this build")
    unique = {}
    for blob in blobs:
        if blob.build_identity != build_identity:
            raise ValueError(f"{blob.source}: mixed build identities")
        identity = (blob.kind, blob.identity_hash)
        previous = unique.get(identity)
        if previous and (
            previous.key != blob.key or previous.fields != blob.fields
            or previous.arrays != blob.arrays or previous.code != blob.code
            or previous.link_sites != blob.link_sites
            or previous.probe_hash != blob.probe_hash
            or previous.slot_table_hash != blob.slot_table_hash
        ):
            raise ValueError(f"{blob.source}: conflicting artifact identity")
        unique[identity] = blob
    blobs = sorted(unique.values(), key=lambda blob: (blob.kind, blob.identity_hash))

    slot_table_hash = blobs[0].slot_table_hash if blobs else 0
    for b in blobs:
        if b.slot_table_hash != slot_table_hash:
            raise ValueError(
                f"{b.source}: recorded against a different slot table "
                f"({b.slot_table_hash:#x} vs {slot_table_hash:#x})"
            )

    build_identity_offset = HEADER_SIZE
    directory_offset = align_up(build_identity_offset + BUILD_IDENTITY_SIZE, ALIGNMENT)
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
            "keySize": b.key_size,
            "fieldsSize": b.fields_size,
            "arraysSize": b.arrays_size,
            "textOffset": text_cursor,
            "textSize": b.code_size,
        }
        cursor = align_up(cursor + b.key_size + b.fields_size + b.arrays_size, ALIGNMENT)
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
        build_identity_offset,
        BUILD_IDENTITY_SIZE,
        directory_offset,
        text_offset,
        text_size,
        image_size,
    )

    buf[build_identity_offset : build_identity_offset + BUILD_IDENTITY_SIZE] = build_identity

    sites = []
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
            e["keySize"],
        )
        b = blobs[i]
        p = e["dataOffset"]
        buf[p : p + b.key_size] = b.key
        p += b.key_size
        buf[p : p + e["fieldsSize"]] = b.fields
        buf[p + e["fieldsSize"] : p + e["fieldsSize"] + e["arraysSize"]] = b.arrays
        t = text_offset + e["textOffset"]
        buf[t : t + e["textSize"]] = b.code
        for code_offset, slot in b.link_sites:
            if code_offset + LINK_SITE_WIDTH > b.code_size:
                raise ValueError(f"{b.source}: link site at {code_offset} is past code")
            sites.append((t + code_offset, slot))

    sites.sort()
    emit_relocs(relocs_path, buf, sites, slot_table_hash)

    with open(out_path, "wb") as f:
        f.write(buf)

    print(
        f"packed {len(blobs)} blob(s) into {out_path} "
        f"({image_size} bytes, text {text_size} bytes, {len(sites)} link site(s))"
    )


def main(argv):
    p = argparse.ArgumentParser(description="Pack .aotb files into AOTImage.bin")
    p.add_argument("record_dir", help="Directory of .aotb files")
    p.add_argument("out", help="Output AOTImage.bin path")
    p.add_argument(
        "--build-identity", action="append", required=True,
        help="Destination native build identity (repeat for multiple link targets)",
    )
    p.add_argument(
        "--relocs",
        default=None,
        help="Output relocation list path (default: AOTImageRelocs.inc beside out)",
    )
    args = p.parse_args(argv)
    relocs = args.relocs or os.path.join(
        os.path.dirname(args.out), "AOTImageRelocs.inc"
    )
    pack(args.record_dir, args.build_identity, args.out, relocs)


if __name__ == "__main__":
    main(sys.argv[1:])
