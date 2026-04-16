#!/usr/bin/env python3

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Remove IC stubs that embed runtime heap pointers (shapes, specific
# objects/functions, proto chains).  These cannot survive AOT
# serialization because the pointers are ephemeral per-process.

import glob
import os
import re

RUNTIME_POINTER_OPS = {
    "GuardShape",
    "GuardMultipleShapes",
    "GuardProto",
    "GuardSpecificObject",
    "GuardSpecificFunction",
    "GuardDynamicSlotIsSpecificObject",
    "GuardFunctionScript",
    "GuardObjectFuseProperty",
    "GuardDOMExpandoMissingOrGuardShape",
    "LoadProtoObject",
    "LoadObject",
    "MetaScriptedThisShape",
}

pattern = re.compile(r"OP\((" + "|".join(RUNTIME_POINTER_OPS) + r")\)")

removed = 0
kept = 0

for file in sorted(glob.glob("IC-*")):
    with open(file, "r") as f:
        content = f.read()
    if pattern.search(content):
        print("Removing: %s" % file)
        os.unlink(file)
        removed += 1
    else:
        kept += 1

print("\nRemoved %d stubs with runtime heap pointers, kept %d shape-free stubs." % (removed, kept))
