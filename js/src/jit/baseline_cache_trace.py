#!/usr/bin/env python3

import argparse
import json


INSTALL_FIELDS = (
    "timestamp_us",
    "pid",
    "script",
    "baseline",
    "code",
    "runtime",
    "source_id",
    "system_realm",
    "self_hosted",
    "debug_instrumentation",
    "immutable_flags",
    "filename_hash",
    "source_start",
    "source_end",
    "line",
    "column",
    "source_hash",
    "source_encoding",
    "bytecode_hash",
    "code_bytes",
    "allocated_code_bytes",
    "metadata_bytes",
    "link_us",
    "filename_hex",
)
COMPILE_FIELDS = ("timestamp_us", "pid", "script", "phase", "duration_us", "success")
DISCARD_FIELDS = ("timestamp_us", "pid", "script", "baseline")
INTEGER_FIELDS = set(INSTALL_FIELDS + COMPILE_FIELDS + DISCARD_FIELDS) - {
    "script",
    "baseline",
    "code",
    "runtime",
    "phase",
    "filename_hex",
}


def parse_line(line, parent_pid):
    _, marker, record = line.partition("BCACHE\t")
    if not marker:
        return None

    version, event, *values = record.rstrip("\r\n").split("\t")
    if version != "1":
        raise ValueError(f"unsupported trace version: {version}")
    fields = {
        "install": INSTALL_FIELDS,
        "compile": COMPILE_FIELDS,
        "discard": DISCARD_FIELDS,
    }[event]
    if len(values) != len(fields):
        raise ValueError(f"{event}: expected {len(fields)} fields, got {len(values)}")

    item = {"event": event, "version": 1}
    for name, value in zip(fields, values):
        item[name] = int(value) if name in INTEGER_FIELDS else value
    item["process_role"] = (
        "parent" if parent_pid is not None and item["pid"] == parent_pid else "unknown"
    )

    if event == "install":
        item["filename"] = bytes.fromhex(item.pop("filename_hex")).decode(
            "utf-8", "surrogateescape"
        )
        item["provenance"] = (
            "self_hosted"
            if item["self_hosted"]
            else "system"
            if item["system_realm"]
            else "guest"
        )
        item["cache_bytes"] = item["allocated_code_bytes"] + item["metadata_bytes"]
        item["cache_key"] = [
            item[name]
            for name in (
                "filename",
                "source_start",
                "source_end",
                "source_encoding",
                "source_hash",
                "bytecode_hash",
                "immutable_flags",
                "debug_instrumentation",
            )
        ]
        item["key_has_source"] = bool(item["source_encoding"])
    return item


def main():
    parser = argparse.ArgumentParser(description="Extract Baseline cache trace records")
    parser.add_argument("logs", nargs="+", help="MOZ_LOG_FILE output files")
    parser.add_argument("--parent-pid", type=int)
    args = parser.parse_args()

    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as stream:
            for number, line in enumerate(stream, 1):
                try:
                    item = parse_line(line, args.parent_pid)
                except (KeyError, ValueError) as error:
                    raise SystemExit(f"{path}:{number}: {error}") from error
                if item is not None:
                    print(json.dumps(item))


if __name__ == "__main__":
    main()
