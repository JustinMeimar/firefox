import glob

ic_list = glob.glob("IC-*.S")
sorted_ic_list = sorted(ic_list)

with open("moz.build", "w") as f:
    f.write(f"""
# -*- Mode: python; indent-tabs-mode: nil; tab-width: 40 -*-
# vim: set filetype=python:
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
# THIS IS A GENERATED FILE. DO NOT MODIFY.
# SEE generate_mozbuild.py

FINAL_LIBRARY = "js"

SOURCES += [
    {",\n    ".join(f'"{x}"' for x in sorted_ic_list)}
]
    """)
