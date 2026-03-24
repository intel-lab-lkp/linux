#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Launch veth BQL test inside virtme-ng
#
# Must be run from the kernel build tree root.
#
# Options:
#   --verbose       Show kernel console (vng boot messages) in real time.
#                   Useful for debugging kernel panics / BUG_ON crashes.
#   All other options are forwarded to veth_bql_test.sh (see --help there).
#
# Examples:
#   ./tools/testing/selftests/net/veth_bql_test_virtme.sh
#   ./tools/testing/selftests/net/veth_bql_test_virtme.sh --duration 20 --nrules 1000 --qdisc fq_codel
#   ./tools/testing/selftests/net/veth_bql_test_virtme.sh --qdisc fq_codel --bql-disable
#   ./tools/testing/selftests/net/veth_bql_test_virtme.sh --verbose --qdisc-replace --duration 60

set -eu

# Parse --verbose (consumed here, not forwarded to the inner test).
VERBOSE=""
INNER_ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--verbose" ]; then
        VERBOSE="--verbose"
    else
        INNER_ARGS+=("$arg")
    fi
done
TEST_ARGS=$(printf '%q ' "${INNER_ARGS[@]+${INNER_ARGS[@]}}")
TEST="tools/testing/selftests/net/veth_bql_test.sh"

if [ ! -f "$TEST" ]; then
    echo "ERROR: Run this from the kernel tree root" >&2
    echo "  cd /path/to/kernel && $0" >&2
    exit 1
fi

# Verify .config has the options needed for virtme-ng and this test.
# Without these the VM silently stalls with no output.
KCONFIG=".config"
if [ ! -f "$KCONFIG" ]; then
    echo "ERROR: No .config found -- build the kernel first" >&2
    exit 1
fi

MISSING=""
for opt in CONFIG_VIRTIO CONFIG_VIRTIO_PCI CONFIG_VIRTIO_NET \
           CONFIG_VIRTIO_CONSOLE CONFIG_NET_9P CONFIG_NET_9P_VIRTIO \
           CONFIG_9P_FS CONFIG_VETH CONFIG_BQL; do
    if ! grep -q "^${opt}=[ym]" "$KCONFIG"; then
        MISSING+="  $opt\n"
    fi
done
if [ -n "$MISSING" ]; then
    echo "ERROR: .config is missing options required by virtme-ng:" >&2
    echo -e "$MISSING" >&2
    echo "Consider: vng --kconfig (or make defconfig + enable above)" >&2
    exit 1
fi

TESTDIR="tools/testing/selftests/net"
TESTNAME="veth_bql_test.sh"
LOGFILE="veth_bql_test.log"
LOGPATH="$TESTDIR/$LOGFILE"
CONSOLELOG="veth_bql_console.log"
rm -f "$LOGPATH" "$CONSOLELOG"

echo "Starting VM... test output in $LOGPATH, kernel console in $CONSOLELOG"
echo "(VM is booting, please wait ~30s)"

# Always capture kernel console to a file via a second QEMU serial port.
# vng claims ttyS0 (mapped to /dev/null); --qemu-opts adds ttyS1 on COM2.
# earlycon registers COM2's I/O port (0x2f8) as a persistent console.
# (plain console=ttyS1 does NOT work: the 8250 driver registers once,
# ttyS0 wins, and ttyS1 is never picked up.)
# --verbose additionally shows kernel console in real time on the terminal.
SERIAL_CONSOLE="earlycon=uart8250,io,0x2f8,115200"
SERIAL_CONSOLE+=" console=uart8250,io,0x2f8,115200"
set +e
vng $VERBOSE --cpus 4 --memory 2G \
    --rwdir "$TESTDIR" \
    --append "panic=5 loglevel=4 $SERIAL_CONSOLE" \
    --qemu-opts="-serial file:$CONSOLELOG" \
    --exec "cd $TESTDIR && \
        ./$TESTNAME $TEST_ARGS 2>&1 | \
        tee $LOGFILE; echo EXIT_CODE=\$? >> $LOGFILE"
VNG_RC=$?
set -e

echo ""
if [ "$VNG_RC" -ne 0 ]; then
    echo "***********************************************************"
    echo "* VM CRASHED -- kernel panic or BUG_ON (vng rc=$VNG_RC)"
    echo "***********************************************************"
    if [ -s "$CONSOLELOG" ] && \
       grep -qiE 'kernel BUG|BUG:|Oops:|panic|dql_completed' "$CONSOLELOG"; then
        echo ""
        echo "--- kernel backtrace ($CONSOLELOG) ---"
        grep -iE -A30 'kernel BUG|BUG:|Oops:|panic|dql_completed' \
            "$CONSOLELOG" | head -50
    else
        echo ""
        echo "Re-run with --verbose to see the kernel backtrace:"
        echo "  $0 --verbose ${INNER_ARGS[*]}"
    fi
    exit 1
elif [ ! -f "$LOGPATH" ]; then
    echo "No log file found -- VM may have crashed before writing output"
    exit 2
else
    echo "=== VM finished ==="
fi

# Scan console log for unexpected kernel warnings (even on clean exit)
if [ -s "$CONSOLELOG" ]; then
    WARN_PATTERN='kernel BUG|BUG:|Oops:|dql_completed|WARNING:|asks to queue packet|NETDEV WATCHDOG'
    WARN_LINES=$(grep -cE "$WARN_PATTERN" "$CONSOLELOG" 2>/dev/null) || WARN_LINES=0
    if [ "$WARN_LINES" -gt 0 ]; then
        echo ""
        echo "*** kernel warnings in $CONSOLELOG ($WARN_LINES lines) ***"
        grep -E "$WARN_PATTERN" "$CONSOLELOG" | head -20
    fi
fi
