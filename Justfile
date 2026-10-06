set positional-arguments

mozconfigs := env_var('HOME') / 'mozconfigs'

default:
    @just --list

# Build the debug shell in build-shell-debug/dist/bin/js.
debug-shell *args:
    direnv exec . env MOZCONFIG="{{mozconfigs}}/shell-debug.mozconfig" MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none ./mach build "$@"

# Build the debug AOT shell (requires ARM64 AOT support in the checkout).
debug-aot-shell *args:
    direnv exec . env MOZCONFIG="{{mozconfigs}}/shell-debug-aot.mozconfig" MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none ./mach build "$@"

# Build the release AOT shell (requires ARM64 AOT support in the checkout).
release-aot-shell *args:
    direnv exec . env MOZCONFIG="{{mozconfigs}}/shell-release-aot.mozconfig" MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none ./mach build "$@"

# Build both AOT shells sequentially to limit memory use.
all-aot: debug-aot-shell release-aot-shell

# Build both shells sequentially to limit memory use.
all: debug-shell debug-aot-shell

configure-debug-shell:
    direnv exec . env MOZCONFIG="{{mozconfigs}}/shell-debug.mozconfig" MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none ./mach configure

configure-debug-aot-shell:
    direnv exec . env MOZCONFIG="{{mozconfigs}}/shell-debug-aot.mozconfig" MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none ./mach configure

configure-release-aot-shell:
    direnv exec . env MOZCONFIG="{{mozconfigs}}/shell-release-aot.mozconfig" MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none ./mach configure

cache-stats:
    direnv exec . sccache --show-stats
