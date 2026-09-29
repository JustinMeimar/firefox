#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Writes an empty valid image for builds that do not have recorded artifacts. An
# image with no artifacts causes the runtime to generate code normally.

import sys

from AOTImageFormat import AOTImageFormat
from PackAOTImage import build_image, encode_relocs


def empty_image(format):
    buf, _ = build_image([], bytes(format.BUILD_IDENTITY_SIZE), format)
    return buf


def main(output, format_path):
    output.write(empty_image(AOTImageFormat.load(format_path)))
    return 0


# A zero slot table hash tells the shim there is nothing to check.
def main_relocs(output, format_path):
    format = AOTImageFormat.load(format_path)
    output.write(encode_relocs(empty_image(format), [], 0, format))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: GenerateEmptyAOTImage.py FORMAT IMAGE RELOCS", file=sys.stderr)
        sys.exit(1)
    with open(sys.argv[2], "wb") as f:
        main(f, sys.argv[1])
    with open(sys.argv[3], "wb") as f:
        main_relocs(f, sys.argv[1])
