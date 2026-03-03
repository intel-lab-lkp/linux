#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

SCRIPT_DIR=$(dirname "$(realpath "$0")")
OUTPUT_DIR="$SCRIPT_DIR/results_$(date +%Y%m%d_%H%M%S)"
TEST_RUNNER="$SCRIPT_DIR/luo_test.sh"

TARGETS=("x86_64" "aarch64")

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=()
FAILED=()
SKIPPED=()

mkdir -p "$OUTPUT_DIR"

TEST_NAMES=(
    "luo_kexec_simple"
    "luo_multi_session"
)

for arch in "${TARGETS[@]}"; do
    for test_name in "${TEST_NAMES[@]}"; do
        log_file="$OUTPUT_DIR/${arch}_${test_name}.log"
        echo -n "  -> $arch $test_name ... "

        if "$TEST_RUNNER" -t "$arch" -T "$test_name" > "$log_file" 2>&1; then
            echo -e "${GREEN}PASS${NC}"
            PASSED+=("${arch}:${test_name}")
        else
            exit_code=$?
            if [ $exit_code -eq 4 ]; then
                echo -e "${YELLOW}SKIP${NC}"
                SKIPPED+=("${arch}:${test_name}")
            else
                echo -e "${RED}FAIL${NC}"
                FAILED+=("${arch}:${test_name}")
            fi
        fi
    done
    echo ""
done

echo "========================================="
echo "             TEST SUMMARY                "
echo "========================================="
echo -e "PASSED: ${GREEN}${#PASSED[@]}${NC}"
echo -e "FAILED: ${RED}${#FAILED[@]}${NC}"
for fail in "${FAILED[@]}"; do
    echo -e "  - $fail"
done
echo -e "SKIPPED: ${YELLOW}${#SKIPPED[@]}${NC}"
echo "Logs: $OUTPUT_DIR"

if [ ${#FAILED[@]} -eq 0 ]; then
    exit 0
else
    exit 1
fi
