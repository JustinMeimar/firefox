#!/bin/bash
# Usage: ./jittest_debug.sh arguments/strict-eval.js

TEST_PATH="$1"
if [ -z "$TEST_PATH" ]; then
    echo "Usage: $0 <test-path>"
    echo "Example: $0 arguments/strict-eval.js"
    exit 1
fi

# Get the full command from jit-test runner
FULL_CMD=$(python3 jit-test/jit_test.py --args="--aot-bl --no-baseline --baseline-eager" ../../jsshell "$TEST_PATH" --show-cmd 2>&1 | grep "^/home" | head -1)

if [ -z "$FULL_CMD" ]; then
    echo "Failed to get command for test: $TEST_PATH"
    exit 1
fi

echo "Running: $FULL_CMD"
echo ""
IONFLAGS=bl-aot gdb --args $FULL_CMD
