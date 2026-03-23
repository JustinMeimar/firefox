
##~---- Build ----~##

build-shell-debug:
    python3 drive.py build --build=build-shell-debug

build-shell-release:
    python3 drive.py build --build=build-shell-release

build-shell-debug-aot:
    python3 drive.py build --build=build-shell-debug-aot

build-shell-release-aot:
    python3 drive.py build --build=build-shell-release-aot

build-browser-release-aot:
    python3 drive.py build --build=build-browser-release-aot

build-all-shells: build-shell-debug build-shell-release build-shell-debug-aot build-shell-release-aot

use BUILD:
    #!/usr/bin/env bash
    target="{{BUILD}}/dist/bin/js"
    [ -f "$target" ] || { echo "Error: $target not found"; exit 1; }
    ln -sfn "$target" jsshell
    echo "jsshell -> $target"

##~---- Run / Debug ----~##

run-simple-aot:
    python3 drive.py run --program ../mozconfigs/progs/simple.js --build=build-shell-debug-aot

debug-simple-aot:
    python3 drive.py debug --program ../mozconfigs/progs/simple.js --build=build-shell-debug-aot

run-aot FILE:
    ./jsshell --use-aot-baseline --blinterp-eager --no-baseline -f {{FILE}}

run-gen FILE:
    ./jsshell --dump-baseline-interpreter --blinterp-eager --no-baseline -f {{FILE}}

dump-aot FILE:
    ./jsshell --dump-baseline-interpreter --blinterp-eager --no-baseline -f {{FILE}}

debug-aot FILE:
    gdb --args ./jsshell --use-aot-baseline --blinterp-eager --no-baseline -f {{FILE}}

debug-aot-expr EXPR:
    gdb --args ./jsshell --use-aot-baseline --blinterp-eager --no-baseline -e "{{EXPR}}"

debug-gen FILE:
    gdb --args ./jsshell --dump-baseline-interpreter --blinterp-eager --no-baseline -f {{FILE}}

move-ics:
    mv $(ls | grep "IC-\d*") ./js/src/ics

##~---- Benchmarks ----~##

BENCH_CPU := "2"

bench-lock:
    #!/usr/bin/env bash
    echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
    echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
    echo 0 | sudo tee /sys/devices/system/cpu/cpu3/online
    echo "locked: boost=off governor=performance cpu3=offline pin=cpu{{BENCH_CPU}}"

bench-unlock:
    #!/usr/bin/env bash
    echo 1 | sudo tee /sys/devices/system/cpu/cpu3/online
    echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
    echo 1 | sudo tee /sys/devices/system/cpu/cpufreq/boost
    echo "unlocked: boost=on governor=powersave cpu3=online"

bench-octane *FLAGS:
    #!/usr/bin/env bash
    cd js/src/octane
    time taskset -c {{BENCH_CPU}} ../../../jsshell {{FLAGS}} -f run.js

bench-octane-compare *ARGS:
    BENCH_CPU={{BENCH_CPU}} python3 scripts/bench_octane.py {{ARGS}}

bench-aot-interp FILE:
    #!/usr/bin/env bash
    echo "=== AOT load ==="
    ./jsshell --aot-bl --blinterp-eager --no-baseline -f "{{FILE}}" 2>&1 | grep -a "\[JIT-timing\]"
    echo ""
    echo "=== JIT generate ==="
    ./jsshell --blinterp-eager --no-baseline --setpref=experimental.self_hosted_cache=true -f "{{FILE}}" 2>&1 | grep -a "\[JIT-timing\]"

##~---- Profiling ----~##

FLAME_DIR := "/home/justin/install/FlameGraph"
PROFILE_DIR := "../profiling"

profile-flame FILE:
    #!/usr/bin/env bash
    timestamp=$(date +%Y%m%d_%H%M%S)
    export IONFLAGS=bl-aot
    perf record -g ./jsshell --aot-bl -f "{{FILE}}"
    perf script | "{{FLAME_DIR}}/stackcollapse-perf.pl" \
                | "{{FLAME_DIR}}/flamegraph.pl" \
                > "{{PROFILE_DIR}}/${timestamp}-flamegraph.svg"
    echo "{{PROFILE_DIR}}/${timestamp}-flamegraph.svg"

##~---- Browser ----~##

BROWSER_AOT_MOZCONFIG := justfile_directory() + "/../mozconfigs/build-browser-release-aot"

jitless-collect-ics:
    #!/home/justin/.nix-profile/bin/zsh
    AOT_ICS_KEEP_GOING=1 MOZ_DISABLE_CONTENT_SANDBOX=1 MOZCONFIG="{{BROWSER_AOT_MOZCONFIG}}" ./mach run

jitless-no-jit:
    AOT_ICS_KEEP_GOING=1 MOZ_DISABLE_CONTENT_SANDBOX=1 MOZCONFIG="{{BROWSER_AOT_MOZCONFIG}}" \
    ./mach run --setpref javascript.options.disabljitbackend=true

jitless-only-blinterp:
    AOT_ICS_KEEP_GOING=1 MOZ_DISABLE_CONTENT_SANDBOX=1 MOZCONFIG="{{BROWSER_AOT_MOZCONFIG}}" ./mach run

jitless-aot-ics:
    MOZCONFIG="{{BROWSER_AOT_MOZCONFIG}}" ./mach run
