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

# run vm-only            --no-jit-backend
# pause
# run aot-blinterp       --no-ion --blinterp-eager --aot-bl --cache-ir-stubs=off
# pause
run aot-blinterp-ics   --no-ion --blinterp-eager --aot-bl --enforce-aot-ics
pause
run aot-selfhosted     --no-ion --blinterp-eager --aot-bl --cache-ir-stubs=off
pause
run aot-selfhosted-ics --no-ion --blinterp-eager --aot-bl --enforce-aot-ics
