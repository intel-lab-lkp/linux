#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Standalone runner for tlob selftests
# Usage: ./run_tlob_tests.sh [options]
#
# Options:
#   -v, --verbose    Verbose output
#   -k, --keep       Keep test logs
#   -l, --logdir DIR Log directory (default: ../../logs)
#   -h, --help       Show this help

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FTRACETEST="$SCRIPT_DIR/../../../ftrace/ftracetest"
LOGDIR="$SCRIPT_DIR/../../logs"
VERBOSE=""
KEEP=""
EXTRA_ARGS=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE="-v"
            shift
            ;;
        -k|--keep)
            KEEP="-k"
            shift
            ;;
        -l|--logdir)
            LOGDIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -v, --verbose    Verbose output"
            echo "  -k, --keep       Keep test logs"
            echo "  -l, --logdir DIR Log directory (default: ../../logs)"
            echo "  -h, --help       Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                           # Run all tlob tests"
            echo "  $0 -v                        # Run with verbose output"
            echo "  $0 -v -l /tmp/tlob-logs      # Custom log directory"
            echo ""
            echo "With vng:"
            echo "  vng -v --rwdir $LOGDIR -- $0"
            exit 0
            ;;
        *)
            EXTRA_ARGS="$EXTRA_ARGS $1"
            shift
            ;;
    esac
done

# Build test helpers
echo "Building tlob test helpers..."
make -C "$SCRIPT_DIR" all

# Check ftracetest exists
if [ ! -x "$FTRACETEST" ]; then
    echo "Error: $FTRACETEST not found or not executable"
    echo "Make sure you're running from the correct directory"
    exit 1
fi

# Create log directory
mkdir -p "$LOGDIR"

# Run tests
echo "Running tlob selftests..."
echo "Log directory: $LOGDIR"
echo ""

# Export RV_BINDIR so test scripts can find tlob_target and tlob_sym
export RV_BINDIR="$SCRIPT_DIR"

# Pass the test directory, not individual .tc files
# ftracetest will discover all .tc files in the directory
"$FTRACETEST" -K $VERBOSE $KEEP --rv --logdir "$LOGDIR" \
    "$SCRIPT_DIR" $EXTRA_ARGS

echo ""
echo "Tests completed. Logs saved to: $LOGDIR"
