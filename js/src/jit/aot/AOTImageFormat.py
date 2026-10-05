# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import json
import struct


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class WireRecord:
    def __init__(self, description):
        self.layout = struct.Struct(description["format"])
        self.names = description["names"]
        self.size = self.layout.size

    def pack_into(self, buffer, offset, **values):
        if values.keys() != set(self.names):
            raise ValueError("wire record fields differ from schema")
        if offset < 0 or offset + self.size > len(buffer):
            raise ValueError("wire record exceeds output buffer")
        self.layout.pack_into(buffer, offset, *(values[name] for name in self.names))

    def unpack_from(self, buffer, offset=0):
        return dict(zip(self.names, self.layout.unpack_from(buffer, offset)))


class AOTImageFormat:
    def __init__(self, description):
        self.__dict__.update(description["constants"])
        self.header = WireRecord(description["records"]["Header"])
        self.directory_entry = WireRecord(description["records"]["DirectoryEntry"])
        self.blob_header = WireRecord(description["records"]["AOTBlobFileHeader"])
        self.link_site = WireRecord(description["records"]["AOTLinkSite"])
        self.blobs = description["blobs"]
        self.kind_names = {i: blob["name"] for i, blob in enumerate(self.blobs)}

    @classmethod
    def load(cls, path):
        with open(path) as stream:
            return cls(json.load(stream))

    def validate(self, kind, fields, arrays, key):
        layout = self.blobs[kind]
        if len(fields) != struct.calcsize(layout["format"]):
            raise ValueError("wrong metadata size")
        metadata = dict(zip(layout["fields"], struct.unpack(layout["format"], fields)))
        cursor = 0
        for array in layout["arrays"]:
            size = metadata[array["name"] + "Count"] * array["size"]
            if cursor % array["alignment"] or size > len(arrays) - cursor:
                raise ValueError(f"invalid metadata array {array['name']}")
            metadata[array["name"]] = arrays[cursor : cursor + size]
            cursor += size
        if cursor != len(arrays):
            raise ValueError("trailing metadata array bytes")
        cursor = 0
        for field in layout["key"]:
            name, kind = field["name"], field["type"]
            if len(key) - cursor < 4:
                raise ValueError(f"truncated compilation key at {name}")
            value = struct.unpack_from("<I", key, cursor)[0]
            cursor += 4
            if kind == "bool" and value > 1:
                raise ValueError(f"invalid boolean {name}")
            if kind in ("bytes", "u32_array"):
                end = cursor + value * (4 if kind == "u32_array" else 1)
                padded = align_up(end, 4)
                if padded > len(key) or any(key[end:padded]):
                    raise ValueError(f"invalid array {name}")
                value = key[cursor:end]
                cursor = padded
            if "metadata" in field and value != metadata[field["metadata"]]:
                raise ValueError(f"compilation input {name} differs from metadata")
        if cursor != len(key):
            raise ValueError("trailing compilation key bytes")
