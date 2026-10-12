#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Tests for the PCIe Lane Margining at the Receiver debugfs interface
# (Documentation/PCI/pcie-lmr.rst).
#
# By default only checks that do not start a margining session are run,
# on every port that has a pcie_lmr_* directory.  To also run a session
# on one link, name a port with the capability:
#
#   PCIE_LMR_DEV=0000:00:01.1 ./pcie_lmr.sh
#
# The link runs with ASPM off for the duration of that test; do not
# point it at a link that is in use for something important.

DIR="$(dirname "$(readlink -f "$0")")"
source "$DIR"/../kselftest/ktap_helpers.sh

DEBUGFS=/sys/kernel/debug
export LC_ALL=C

# Like ktap_test_result, but passes arguments that contain spaces intact
check()
{
	local desc="$1"

	shift
	if "$@"; then
		ktap_test_pass "$desc"
	else
		ktap_test_fail "$desc"
	fi
}

# write_expect <file> <value> <expected>
# <expected> is "ok" or the strerror() text of the expected error.
write_expect()
{
	local file="$1" val="$2" want="$3" err

	err=$( { printf '%s' "$val" > "$file"; } 2>&1 )
	if [ $? -eq 0 ]; then
		[ "$want" = "ok" ]
		return
	fi
	[ "$want" != "ok" ] && [[ "$err" == *"$want"* ]]
}

# read_expect <file> <expected>, like write_expect
read_expect()
{
	local file="$1" want="$2" err

	err=$( { cat "$file" > /dev/null; } 2>&1 )
	if [ $? -eq 0 ]; then
		[ "$want" = "ok" ]
		return
	fi
	[ "$want" != "ok" ] && [[ "$err" == *"$want"* ]]
}

# caps_get <port dir> <key>
caps_get()
{
	sed -n "s/^$2: //p" "$1/lane0/caps"
}

check_layout()
{
	local d="$1" f

	for f in enable receiver port lane0/caps lane0/margin_timing \
		 lane0/margin_voltage lane0/status; do
		[ -e "$d/$f" ] || return 1
	done
}

check_port_file()
{
	local out

	out=$(cat "$1/port") || return 1
	[[ "$out" == *"uses_driver_software: "[01]* ]] &&
	[[ "$out" == *"margining_ready: "[01]* ]] &&
	[[ "$out" == *"software_ready: "[01]* ]]
}

PASSIVE_TESTS=9

# Checks that never start a session
passive_tests()
{
	local d="$1" n i

	n=$(basename "$d")

	check "$n: files present" check_layout "$d"
	check "$n: port file readable" check_port_file "$d"

	if [ "$(cat "$d/enable" 2>/dev/null)" != "0" ]; then
		for i in $(seq 3 $PASSIVE_TESTS); do
			ktap_test_skip "$n: session active, test $i skipped"
		done
		return
	fi

	check "$n: enable rejects '2'" \
		write_expect "$d/enable" 2 "Invalid argument"
	check "$n: enable rejects 'x'" \
		write_expect "$d/enable" x "Invalid argument"
	check "$n: enable=0 without a session is accepted" \
		write_expect "$d/enable" 0 ok
	check "$n: margin_timing rejects 'abc'" \
		write_expect "$d/lane0/margin_timing" abc "Invalid argument"
	check "$n: margin_timing needs a session" \
		write_expect "$d/lane0/margin_timing" 1 \
		"Transport endpoint is not connected"
	check "$n: caps needs a session" \
		read_expect "$d/lane0/caps" "Transport endpoint is not connected"
	check "$n: receiver needs a session" \
		write_expect "$d/receiver" 1 "Transport endpoint is not connected"
}

ACTIVE_TESTS=15

# Print the device at the other end of the link of PCI device $1.
# Receiver 6 is the child device (PCIe Upstream Port) below the Downstream
# Port; receiver 1 is the Downstream Port (parent bridge).
partner_of()
{
	local sys=/sys/bus/pci/devices/$1 rx c

	rx=$(cat "$DEBUGFS/pcie_lmr_$1/receiver" 2> /dev/null)
	if [ "$rx" = 6 ]; then
		basename "$(dirname "$(readlink -f "$sys")")"
		return
	fi
	for c in "$sys"/[0-9a-fA-F]*:??:??.0; do
		[ -e "$c/config" ] && { basename "$c"; return; }
	done
}

# Link Control and Link Control 2 of PCI device $1, or nothing
link_regs()
{
	[ -n "$1" ] && command -v setpci > /dev/null || return
	setpci -s "$1" CAP_EXP+10.w CAP_EXP+30.w 2> /dev/null | paste -sd ' '
}

check_bad_rx()
{
	local d="$1" rx="$2" r

	write_expect "$d/receiver" 0 "Invalid argument" || return 1
	write_expect "$d/receiver" 7 "Invalid argument" || return 1
	if [ "$rx" = 1 ]; then
		write_expect "$d/receiver" 6 "Invalid argument"
		return
	fi
	for r in 1 2 3 4 5; do
		write_expect "$d/receiver" "$r" "Invalid argument" || return 1
	done
}

check_step_roundtrip()
{
	write_expect "$1/lane0/margin_timing" 1 ok &&
	[[ "$(cat "$1/lane0/status")" == timing* ]] &&
	write_expect "$1/lane0/margin_timing" 0 ok &&
	test "$(cat "$1/lane0/margin_timing")" = 0 &&
	test "$(cat "$1/lane0/status")" = idle
}

# Highest valid receiver number on Downstream Port $1 (debugfs dir $2):
# 1 with no retimers, 3 with one retimer, 5 with two retimers.
retimer_max_rx()
{
	local dev="$1" d="$2" lnksta2 retimers

	if command -v setpci > /dev/null; then
		lnksta2=$(setpci -s "$dev" CAP_EXP+32.w 2> /dev/null)
		if [ -n "$lnksta2" ]; then
			retimers=$(( ((0x$lnksta2 & 0x40) != 0) +
				     ((0x$lnksta2 & 0x80) != 0) ))
			echo $((1 + 2 * retimers))
			return
		fi
	fi
	if write_expect "$d/receiver" 4 ok; then
		echo 5
	elif write_expect "$d/receiver" 2 ok; then
		echo 3
	else
		echo 1
	fi
}

check_one_retimer_rx()
{
	local d="$1" r="$2" rsteps

	write_expect "$d/receiver" "$r" ok &&
	test "$(cat "$d/receiver")" = "$r" &&
	test "$(caps_get "$d" receiver)" = "$r" || return 1

	rsteps=$(caps_get "$d" timing_steps)
	[ -n "$rsteps" ] || return 1
	if [ "$rsteps" -ge 1 ]; then
		check_step_roundtrip "$d" || return 1
	fi
}

# Discover retimers on Downstream Port $1 (debugfs dir $2) and test each
# retimer receiver (2 and 3 for the first retimer, 4 and 5 for the second),
# while verifying that absent retimer receivers are rejected.
check_retimers()
{
	local dev="$1" d="$2" max_rx r rc=0

	max_rx=$(retimer_max_rx "$dev" "$d")
	for r in 2 3 4 5; do
		if [ "$r" -le "$max_rx" ]; then
			check_one_retimer_rx "$d" "$r" || { rc=1; break; }
		else
			write_expect "$d/receiver" "$r" \
				"Invalid argument" || { rc=1; break; }
		fi
	done
	write_expect "$d/receiver" 1 ok &&
	test "$(cat "$d/receiver")" = 1 &&
	return "$rc"
}

check_end_session()
{
	write_expect "$1/enable" 0 ok &&
	test "$(cat "$1/enable")" = 0
}

stop_session()
{
	[ -n "$ACTIVE_DIR" ] && echo 0 > "$ACTIVE_DIR/enable" 2> /dev/null
}

skip_rest()
{
	while [ "$KTAP_TESTNO" -le "$KSFT_NUM_TESTS" ]; do
		ktap_test_skip "$1"
	done
}

active_tests()
{
	local dev="$1" d="$DEBUGFS/pcie_lmr_$1" partner before after
	local rx steps child

	partner=$(partner_of "$dev")
	if [ -z "$partner" ]; then
		ktap_test_fail "$dev: start a session"
		skip_rest "$dev: no link partner"
		return
	fi
	before="$(link_regs "$dev") $(link_regs "$partner")"

	ACTIVE_DIR="$d"
	trap stop_session EXIT INT TERM

	if ! write_expect "$d/enable" 1 ok; then
		ktap_test_fail "$dev: start a session"
		skip_rest "$dev: no session"
		return
	fi
	ktap_test_pass "$dev: start a session"

	check "$dev: enable=1 again is accepted" \
		write_expect "$d/enable" 1 ok
	check "$dev: enable reads 1" \
		test "$(cat "$d/enable")" = 1

	if [ -n "$partner" ] && [ -d "$DEBUGFS/pcie_lmr_$partner" ]; then
		check "$dev: partner port rejects a second session" \
			write_expect "$DEBUGFS/pcie_lmr_$partner/enable" 1 \
			"Device or resource busy"
	else
		ktap_test_skip "$dev: partner has no pcie_lmr directory"
	fi

	rx=$(cat "$d/receiver")
	check "$dev: default receiver is 1 or 6" \
		test "$rx" = 1 -o "$rx" = 6
	check "$dev: unreachable receivers are rejected" \
		check_bad_rx "$d" "$rx"
	check "$dev: receiver 256 is rejected" \
		write_expect "$d/receiver" 256 "Numerical result out of range"

	steps=$(caps_get "$d" timing_steps)
	check "$dev: caps reports timing_steps" \
		test -n "$steps"
	check "$dev: timing step above timing_steps is rejected" \
		write_expect "$d/lane0/margin_timing" $((steps + 1)) \
		"Numerical result out of range"

	if [ "${steps:-0}" -ge 1 ]; then
		check "$dev: timing step 1 then 0" \
			check_step_roundtrip "$d"
	else
		ktap_test_skip "$dev: receiver reports no timing steps"
	fi

	if [ "$(caps_get "$d" independent_left_right_timing)" = 0 ]; then
		check "$dev: negative timing step is rejected" \
			write_expect "$d/lane0/margin_timing" -1 "Invalid argument"
	else
		ktap_test_skip "$dev: receiver supports left/right timing"
	fi

	if [ "$(caps_get "$d" voltage_supported)" = 0 ]; then
		check "$dev: voltage step is rejected" \
			write_expect "$d/lane0/margin_voltage" 1 \
			"Operation not supported"
	else
		ktap_test_skip "$dev: receiver supports voltage margining"
	fi

	if [ "$rx" = 1 ]; then
		check "$dev: retimer receivers" \
			check_retimers "$dev" "$d"
	else
		ktap_test_skip "$dev: retimers are margined from the Downstream Port"
	fi

	if check_end_session "$d"; then
		ktap_test_pass "$dev: end the session"
		ACTIVE_DIR=
	else
		ktap_test_fail "$dev: end the session"
	fi

	# In drivers/pci/pcie/aspm.c, aspm_ctrl_attrs_are_visible() uses
	# pcie_aspm_get_link(pdev) -> pci_upstream_bridge(pdev)->link_state,
	# so the sysfs 'link/' directory is attached to the child device
	# below the Downstream Port (the PCIe Upstream Port, receiver 6),
	# not to the Downstream Port itself.
	child=$([ "$rx" = 6 ] && echo "$dev" || echo "$partner")
	after="$(link_regs "$dev") $(link_regs "$partner")"
	if [ -z "${before// }" ]; then
		ktap_test_skip "$dev: setpci not found"
	elif [ -n "$child" ] && [ ! -d "/sys/bus/pci/devices/$child/link" ]; then
		ktap_test_skip "$dev: no OS ASPM control on link"
	else
		check "$dev: LNKCTL and LNKCTL2 restored on both ends" \
			test "$before" = "$after"
	fi
}

ktap_print_header

if [ "$(id -u)" -ne 0 ]; then
	ktap_skip_all "must be run as root"
	exit "$KSFT_SKIP"
fi

mountpoint -q "$DEBUGFS" || mount -t debugfs none "$DEBUGFS" 2> /dev/null

ports=("$DEBUGFS"/pcie_lmr_*)
if [ ! -d "${ports[0]}" ]; then
	ktap_skip_all "no pcie_lmr_* directories in debugfs"
	exit "$KSFT_SKIP"
fi

if [ -n "$PCIE_LMR_DEV" ] && [ ! -d "$DEBUGFS/pcie_lmr_$PCIE_LMR_DEV" ]; then
	ktap_exit_fail_msg "no pcie_lmr directory for $PCIE_LMR_DEV"
fi

plan=$(( ${#ports[@]} * PASSIVE_TESTS ))
[ -n "$PCIE_LMR_DEV" ] && plan=$((plan + ACTIVE_TESTS))
ktap_set_plan "$plan"

for d in "${ports[@]}"; do
	passive_tests "$d"
done

if [ -n "$PCIE_LMR_DEV" ]; then
	active_tests "$PCIE_LMR_DEV"
fi

ktap_finished
