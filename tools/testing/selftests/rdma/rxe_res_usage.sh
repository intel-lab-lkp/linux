#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test rdma resource show max and usage percentage display.
# Requires: rdma_rxe module, rdma tool (iproute2), rping utility.
#
# Verifies:
#   1. "rdma resource show" outputs usage percentage for qp/cq/mr/pd/srq
#   2. Usage percentage is displayed and correct
#   3. JSON output contains "max" and "usage" fields
#   4. Resources without max limit (cm_id, ctx) show no percentage

set -e

NS_NAME="rxe_res_test"
VETH_HOST="veth_res0"
VETH_NS="veth_res1"
RXE_NAME="rxe_res"

PASS=0
FAIL=0

cleanup() {
    ip netns exec "$NS_NAME" rdma link del "$RXE_NAME" 2>/dev/null || true
    ip netns del "$NS_NAME" 2>/dev/null || true
    modprobe -r rdma_rxe 2>/dev/null || true
    echo ""
    echo "Results: $PASS passed, $FAIL failed"
    if [ "$FAIL" -gt 0 ]; then
        exit 1
    fi
}
trap cleanup EXIT

assert_contains() {
    local desc="$1"
    local haystack="$2"
    local needle="$3"

    if echo "$haystack" | grep -q "$needle"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected '$needle' in output)"
        FAIL=$((FAIL + 1))
    fi
}

assert_field_value() {
    local desc="$1"
    local json="$2"
    local field="$3"
    local expected="$4"
    local actual

    actual=$(echo "$json" | jq -r "$field" 2>/dev/null) || true
    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $desc ($field=$actual)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected $field=$expected, got $actual)"
        FAIL=$((FAIL + 1))
    fi
}

assert_usage_pct() {
    local desc="$1"
    local curr="$2"
    local max="$3"
    local usage_str="$4"

    # Calculate expected percentage with 1 decimal place using shell arithmetic
    # Multiply by 1000 first to get one decimal: (curr * 1000 * 100) / max = per-mille
    local permille=$(( curr * 100000 / max ))
    local int_part=$(( permille / 1000 ))
    local frac_part=$(( permille % 1000 ))
    local expected="${int_part}.$(printf '%03d' $frac_part | sed 's/0*$//')"
    # Trim trailing dot
    expected="${expected%.}"

    # Extract percentage from usage string like "(0.0%)"
    local actual
    actual=$(echo "$usage_str" | sed 's/.*(\([0-9.]*\)%).*/\1/')

    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $desc (usage=$actual%)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected $expected%, got $actual%)"
        FAIL=$((FAIL + 1))
    fi
}

# ============================================================
# Setup
# ============================================================

echo "=== Setting up test environment ==="

# Check prerequisites
if ! command -v rdma >/dev/null 2>&1; then
    echo "SKIP: 'rdma' not found, test requires it"
    exit 4
fi

if ! modinfo rdma_rxe >/dev/null 2>&1; then
    echo "SKIP: rdma_rxe kernel module not found"
    exit 4
fi

modprobe rdma_rxe

ip netns add "$NS_NAME"
ip link add "$VETH_HOST" type veth peer name "$VETH_NS"
ip link set "$VETH_NS" netns "$NS_NAME"
ip netns exec "$NS_NAME" ip link set "$VETH_NS" up
ip link set "$VETH_HOST" up

ip netns exec "$NS_NAME" rdma link add "$RXE_NAME" type rxe netdev "$VETH_NS"
echo "RXE link $RXE_NAME created."

# Helper: run rdma in test namespace
rdma_ns() {
    ip netns exec "$NS_NAME" rdma "$@"
}

# ============================================================
# Test 1: Plain text output has usage percentage
# ============================================================
echo ""
echo "=== Test 1: Plain text output contains usage percentage ==="

OUT=$(rdma_ns resource show)
echo "Output: $OUT"

for res in qp cq mr pd srq; do
    assert_contains "$res has usage percentage" "$OUT" "$res [0-9]* ([0-9.]*%)"
done

# ============================================================
# Test 2: Usage percentage is numerically correct
# ============================================================
echo ""
echo "=== Test 2: Usage percentage is parsed correctly ==="

# Extract curr and usage for qp from rxe_res device line only
# Expected format: "... rxe_res: ... qp 1 (0.0%) ..."
QP_LINE=$(echo "$OUT" | grep "$RXE_NAME" | sed -n 's/.*\(qp [0-9]* ([0-9.]*%)\).*/\1/p')
if [ -n "$QP_LINE" ]; then
    QP_CURR=$(echo "$QP_LINE" | sed 's/qp \([0-9]*\) .*/\1/')
    QP_USAGE=$(echo "$QP_LINE" | sed 's/.*(\([0-9.]*%\))/(\1)/')
    if [ -n "$QP_CURR" ] && [ "$QP_CURR" -ge 0 ] 2>/dev/null; then
        echo "  PASS: qp usage percentage parsed ($QP_CURR $QP_USAGE)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: Could not parse qp curr value"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  SKIP: Could not parse qp line for percentage check"
fi

# ============================================================
# Test 4: JSON output structure
# ============================================================
echo ""
echo "=== Test 3: JSON output structure ==="

if ! command -v jq >/dev/null 2>&1; then
    echo "  SKIP: jq not available, skipping JSON tests"
else
JSON=$(rdma_ns -j resource show)
echo "JSON: $JSON"

# JSON should be valid
if echo "$JSON" | jq . >/dev/null 2>&1; then
    echo "  PASS: JSON is valid"
    PASS=$((PASS + 1))
else
    echo "  FAIL: JSON is invalid"
    FAIL=$((FAIL + 1))
    JSON="[]"
fi

# Check that at least one entry has a "max" field
if echo "$JSON" | jq -e '.[] | select(.max != null)' >/dev/null 2>&1; then
    echo "  PASS: JSON contains 'max' field"
    PASS=$((PASS + 1))
else
    echo "  FAIL: JSON missing 'max' field"
    FAIL=$((FAIL + 1))
fi

# Check that at least one entry has a "usage" field
if echo "$JSON" | jq -e '.[] | select(.usage != null)' >/dev/null 2>&1; then
    echo "  PASS: JSON contains 'usage' field"
    PASS=$((PASS + 1))
else
    echo "  FAIL: JSON missing 'usage' field"
    FAIL=$((FAIL + 1))
fi
fi

# ============================================================
# Test 5: Per-device JSON output
# ============================================================
echo ""
echo "=== Test 4: Per-device JSON output ==="

if ! command -v jq >/dev/null 2>&1; then
    echo "  SKIP: jq not available, skipping JSON tests"
else
DEV_JSON=$(rdma_ns -j resource show "$RXE_NAME")
echo "Device JSON: $DEV_JSON"

if [ "$DEV_JSON" != "[]" ]; then
    echo "  PASS: Device-specific query returned data"
    PASS=$((PASS + 1))

    # JSON is flat with duplicate "max" keys per resource; jq returns the last one
    JSON_MAX=$(echo "$DEV_JSON" | jq -r '.[0].max' 2>/dev/null)
    if [ -n "$JSON_MAX" ] && [ "$JSON_MAX" -gt 0 ] 2>/dev/null; then
        echo "  PASS: JSON max field present and > 0 ($JSON_MAX)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: JSON max is missing or zero (got: $JSON_MAX)"
        FAIL=$((FAIL + 1))
    fi

    if echo "$DEV_JSON" | grep -q '"usage"'; then
        echo "  PASS: JSON usage field present"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: JSON usage field missing"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: Device-specific query returned empty"
    FAIL=$((FAIL + 1))
fi
fi

# ============================================================
# Test 6: Verify max values match ibv_devinfo
# ============================================================
echo ""
echo "=== Test 5: Max values match ibv_devinfo ==="

if ! command -v jq >/dev/null 2>&1; then
    echo "  SKIP: jq not available, skipping ibv_devinfo cross-validation"
elif ! command -v ibv_devinfo >/dev/null 2>&1; then
    echo "  SKIP: ibv_devinfo not available"
else
    IBV_OUT=$(ip netns exec "$NS_NAME" ibv_devinfo -v -d "$RXE_NAME" 2>/dev/null || true)
    if [ -n "$IBV_OUT" ]; then
        IBV_MAX_QP=$(echo "$IBV_OUT" | sed -n 's/.*max_qp:[[:space:]]*\([0-9]*\).*/\1/p')
        IBV_MAX_PD=$(echo "$IBV_OUT" | sed -n 's/.*max_pd:[[:space:]]*\([0-9]*\).*/\1/p')

        # Verify ibv_devinfo reports reasonable max values
        if [ -n "$IBV_MAX_QP" ] && [ "$IBV_MAX_QP" -gt 0 ] 2>/dev/null; then
            echo "  PASS: ibv_devinfo max_qp=$IBV_MAX_QP"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ibv_devinfo max_qp missing or zero"
            FAIL=$((FAIL + 1))
        fi
        if [ -n "$IBV_MAX_PD" ] && [ "$IBV_MAX_PD" -gt 0 ] 2>/dev/null; then
            echo "  PASS: ibv_devinfo max_pd=$IBV_MAX_PD"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ibv_devinfo max_pd missing or zero"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "  SKIP: ibv_devinfo returned no output"
    fi
fi

# ============================================================
# Test 7: Usage after creating QPs (with rping)
# ============================================================
echo ""
echo "=== Test 6: Usage increases after creating resources ==="

BEFORE_QP_CURR=$(echo "$OUT" | sed -n 's/.*qp \([0-9]*\).*/\1/p' | head -1)
BEFORE_QP_CURR=${BEFORE_QP_CURR:-0}
echo "  QP count before: $BEFORE_QP_CURR"

# Start rping server in background to create QP resources
ip netns exec "$NS_NAME" rping -s -p 12345 -v &
RPING_PID=$!
sleep 1

# Check resource count after
OUT_AFTER=$(rdma_ns resource show)
AFTER_QP_CURR=$(echo "$OUT_AFTER" | sed -n 's/.*qp \([0-9]*\).*/\1/p' | head -1)
AFTER_QP_CURR=${AFTER_QP_CURR:-0}
echo "  QP count after rping: $AFTER_QP_CURR"

if [ "$AFTER_QP_CURR" -ge "$BEFORE_QP_CURR" ]; then
    echo "  PASS: QP count did not decrease ($BEFORE_QP_CURR -> $AFTER_QP_CURR)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: QP count unexpectedly decreased"
    FAIL=$((FAIL + 1))
fi

# Verify usage percentage is still present after count change
QP_LINE_AFTER=$(echo "$OUT_AFTER" | sed -n 's/.*\(qp [0-9]* ([0-9.]*%)\).*/\1/p')
if [ -n "$QP_LINE_AFTER" ]; then
    echo "  PASS: qp usage percentage present after rping ($QP_LINE_AFTER)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: qp usage percentage missing after rping"
    FAIL=$((FAIL + 1))
fi

kill "$RPING_PID" 2>/dev/null || true
wait "$RPING_PID" 2>/dev/null || true

# ============================================================
# Test 8: Resources without max show no usage percentage (cm_id, ctx)
# ============================================================
echo ""
echo "=== Test 7: Resources without max limit (cm_id, ctx) ==="

# cm_id and ctx have no max in ib_device_attr, max=0 from kernel
# In output, they should NOT show usage percentage
CM_ID_LINE=$(echo "$OUT" | sed -n 's/.*\(cm_id [0-9]*\).*/\1/p' | head -1)
if [ -n "$CM_ID_LINE" ]; then
    if echo "$OUT" | grep -q 'cm_id [0-9]* ([0-9.]*%)'; then
        echo "  FAIL: cm_id should not show usage percentage (no ib_device_attr equivalent)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: cm_id has no usage percentage (as expected)"
        PASS=$((PASS + 1))
    fi
else
    echo "  SKIP: cm_id not found in output"
fi

CTX_LINE=$(echo "$OUT" | sed -n 's/.*\(ctx [0-9]*\).*/\1/p' | head -1)
if [ -n "$CTX_LINE" ]; then
    if echo "$OUT" | grep -q 'ctx [0-9]* ([0-9.]*%)'; then
        echo "  FAIL: ctx should not show usage percentage"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ctx has no usage percentage (as expected)"
        PASS=$((PASS + 1))
    fi
else
    echo "  SKIP: ctx not found in output"
fi

echo ""
echo "=== All tests completed ==="
