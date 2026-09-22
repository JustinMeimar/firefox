# Baseline cache trace

Start with a release JS shell. Its JS logger writes to stderr:

```sh
MOZ_LOG=baselineCache:4 obj-release/dist/bin/js workload.js 2> /tmp/baseline-shell.log
python3 js/src/jit/baseline_cache_trace.py /tmp/baseline-shell.log > /tmp/baseline-shell.jsonl
```

Replace `obj-release` with the release object's actual directory. For the
browser, set `MOZ_LOG=baselineCache:4,sync` and
`MOZ_LOG_FILE=/tmp/baseline-cache%PID` before launch. The `%PID` token gives
each process a separate log file, including processes that reuse a child slot.
The `sync` option flushes complete records before the browser exits. Keep all
log files from a run together.

For example:

```sh
MOZ_LOG=baselineCache:4,sync MOZ_LOG_FILE=/tmp/baseline-cache%PID ./mach run
python3 js/src/jit/baseline_cache_trace.py /tmp/baseline-cache-*.moz_log > /tmp/baseline-cache.jsonl
```

Each `BCACHE` record contains a wall-clock timestamp in microseconds and a PID.
The `install` record gives the script and runtime addresses, source ID, source
location, provenance, content hashes, and byte sizes. The script address is only
an instance identifier within one process lifetime. The source ID is likewise
process-local. `compile` records give main-thread or helper-thread backend
duration and success. `link_us` gives main-thread code linking and installation
time. `discard` records mark removal of an installed Baseline script.

The cache key in the extracted JSON is a **candidate recurrence key**, not a
validated key for sharing native code. It combines filename, source extent,
source-text span hash, bytecode hash, immutable script flags, and debug
instrumentation. When source text is unavailable, `key_has_source` is false;
exclude those records from exact-hit estimates. Source text encoding is part of
the key, so equivalent UTF-8 and UTF-16 inputs may count as different scripts.
The original source is never written. The filename is written in the log as hex
and decoded by the extractor.

`code_bytes` counts machine instructions. `allocated_code_bytes` counts the
JitCode allocation, including its header and buffer. `metadata_bytes` counts
the BaselineScript allocation. Use `cache_bytes` for capacity simulation. IC
stubs and their attachment time are outside this trace.

Records from different files may be interleaved in time. Sort extracted records
by `timestamp_us` before replay. A cache entry becomes available at its
`install` event, not when compilation starts. Provide `--parent-pid PID` to the
extractor when the main browser PID is known; other process roles remain
`unknown`. Use `provenance` to separate guest, system-realm, and self-hosted code.

Logging and source hashing are disabled unless the module is enabled. When
enabled, hashing and synchronous logging add overhead; use compilation durations
as relative weights, not as uninstrumented timing measurements.
