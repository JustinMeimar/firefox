# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import hashlib
import os
import shlex
import subprocess
import sys
from pathlib import Path


def main(manifest, source, destination, ar="ar"):
    digest = hashlib.sha256(b"AmberMonkey native build identity v1\0")
    seen = set()

    def add(path):
        path = Path(path).resolve()
        if path in seen:
            return
        seen.add(path)
        digest.update(os.fsencode(path) + b"\0")
        digest.update(path.stat().st_size.to_bytes(8, "little"))
        with path.open("rb") as stream:
            thin = stream.read(8) == b"!<thin>\n"
            stream.seek(0)
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
        if thin:
            members = subprocess.check_output(
                [*shlex.split(ar), "t", str(path)], text=True
            ).splitlines()
            for member in members:
                add(path.parent / member)

    for name in Path(manifest).read_text().splitlines():
        add(name)
    identity = digest.digest()
    Path(source).write_text(
        'extern "C" {\nextern const unsigned char aot_build_identity[32] = {'
        + ",".join(str(byte) for byte in identity)
        + "};\n}\n"
    )
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(f".tmp.{os.getpid()}")
    temporary.write_bytes(identity)
    temporary.replace(destination)


if __name__ == "__main__":
    main(*sys.argv[1:])
