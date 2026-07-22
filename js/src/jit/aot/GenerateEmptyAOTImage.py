#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Emits an empty-but-valid AOTImage.bin into the objdir at build time.
# The runtime loader (js/src/jit/AOTImage.cpp) reads this via .incbin
# and, seeing zero blobs, falls back to runtime codegen. Overwritten
# in stage 2 by `mach jit-aot build` after the shell has recorded a
# real corpus.

import struct
import sys

# Mirror js/src/jit/AOTImage.h::image constants.
IMAGE_MAGIC = 0x49544F41  # "AOTI"
IMAGE_VERSION = 1
FINGERPRINT_SIZE = 20
ALIGNMENT = 16
TEXT_ALIGNMENT = 4096
HEADER_FMT = "<IHHIIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 36


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def emit(fp):
    fingerprint_offset = HEADER_SIZE
    directory_offset = align_up(
        fingerprint_offset + FINGERPRINT_SIZE, ALIGNMENT
    )
    data_end = directory_offset  # no entries, no data
    text_offset = align_up(data_end, TEXT_ALIGNMENT)
    text_size = 0
    image_size = text_offset

    buf = bytearray(image_size)
    struct.pack_into(
        HEADER_FMT,
        buf,
        0,
        IMAGE_MAGIC,
        IMAGE_VERSION,
        0,
        0,  # blobCount
        fingerprint_offset,
        FINGERPRINT_SIZE,
        directory_offset,
        text_offset,
        text_size,
        image_size,
    )
    # Fingerprint left as 20 zero bytes: sentinel meaning "empty".
    fp.write(bytes(buf))


def main(output):
    emit(output)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: GenerateEmptyAOTImage.py OUT", file=sys.stderr)
        sys.exit(1)
    with open(sys.argv[1], "wb") as f:
        emit(f)
