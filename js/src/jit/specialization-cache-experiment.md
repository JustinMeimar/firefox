# Ion specialization recurrence instrumentation

The maintained experiment and analysis live in the adjacent
`ambermonkey/fossils-II/fossils/0-2-specialization-trace` directory. See its
`README.md` for the measurement model, limitations and browser validation steps.
`specialization_cache.py` here forwards to that analyzer; `SPECIALIZATION_FOSSIL`
can override its location.

Enable `MOZ_LOG=specializationCache:4,sync,append` from startup and set
`JS_SPECIALIZATION_BUILD_ID` to an identifier for the exact binary/configuration.
The browser runner hashes the binary, libxul and mozinfo automatically. Retain
all child logs: forked processes may share a file. Version 2 frames JSON into
short `SPCACHE2` records with process identity, sequence and fragment indices.
The extractor rejects missing or duplicate fragments and v1 logs.

The patch records only Ion compilation inputs selected by Warp, root/inlinee
content identities, dependency types, phase costs, link results and local
bailout/invalidation events. Identity initialization is lazy. It no longer logs
Baseline or IC histories and has no eager-compilation hint intervention.

Projection recurrence is compiler-input evidence, not safe native-code reuse.
Pointer erasure leaves binding obligations unresolved. Inline topology and
optimizer-introduced guards are not modeled. The unchanged Baseline-cache
logger remains available for the separate Baseline experiment.
