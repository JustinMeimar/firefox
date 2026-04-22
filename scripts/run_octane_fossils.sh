#!/usr/bin/env bash
set -euo pipefail

BENCH_CPU="${BENCH_CPU:-2}"
SHELL_BIN="./jsshell"
OCTANE="js/src/octane/run.js"
N=1

run() {
    local tag="$1"; shift
    echo "=== $tag ==="
    fossil bury octane --tag "$tag" -n "$N" -- \
        cd js/src/octane '&&' taskset -c "$BENCH_CPU" ../../../"$SHELL_BIN" "$@" -f run.js
}

pause() {
    echo ""
    read -rp "Press enter to continue to next variant (or Ctrl-C to stop)... "
    echo ""
}

run vm-only            --no-jit-backend
pause
run aot-interp         --no-ion --no-baseline --blinterp-eager --aot-interp --cache-ir-stubs=off
pause
run aot-interp-ics     --no-ion --no-baseline --blinterp-eager --aot-interp --aot-ics --enforce-aot-ics
pause
run aot-interp-sh      --no-ion --no-baseline --blinterp-eager --aot-interp --aot-selfhosted --cache-ir-stubs=off
pause
run aot-interp-sh-ics  --no-ion --no-baseline --blinterp-eager --aot-bl --enforce-aot-ics
