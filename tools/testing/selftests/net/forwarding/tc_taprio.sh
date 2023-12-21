#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ALL_TESTS="in_band out_of_band cycle_extension"
NUM_NETIFS=2
VETH_OPTS="numtxqueues 8 numrxqueues 8"
source lib.sh
source tsn_lib.sh

in_band()
{
	local basetime=$(clock_gettime CLOCK_REALTIME)
	local window_size=$((NSEC_PER_SEC / 2))
	local cycletime=$((2 * window_size))
	local expected=1
	local isochron_dat="$(mktemp)"
	local window_start
	local window_end

	basetime=$((basetime + UTC_TAI_OFFSET * NSEC_PER_SEC))
	basetime=$(round_up_with_margin $basetime $NSEC_PER_SEC $NSEC_PER_SEC)

	tc qdisc replace dev $h1 root stab overhead 24 taprio num_tc 2 \
		map 0 1 \
		queues 1@0 1@1 \
		base-time $basetime \
		sched-entry S 0x3 500000000 \
		sched-entry S 0x0 500000000 \
		clockid CLOCK_TAI \
		flags 0x0

	isochron_do \
		$h1 $h2 \
		"" "" \
		$((basetime + 2 * cycletime)) \
		$cycletime \
		0 \
		${expected} \
		"" \
		1 \
		"" \
		"--omit-hwts --taprio --window-size $window_size" \
		"--omit-hwts" \
		"${isochron_dat}"

	# Count all received packets by looking at the non-zero RX timestamps
	received=$(isochron report \
		--input-file "${isochron_dat}" \
		--printf-format "%u\n" --printf-args "r" | \
		grep -w -v '0' | wc -l)

	if [ "${received}" = "${expected}" ]; then
		RET=0
	else
		RET=1
		echo "Expected isochron to receive ${expected} packets but received ${received}"
	fi

	tx_tstamp=$(isochron report \
		--input-file "${isochron_dat}" \
		--printf-format "%u\n" --printf-args "t")

	window_start=$((basetime + 2 * cycletime))
	window_end=$((window_start + window_size))

	if (( tx_tstamp >= window_start && tx_tstamp <= window_end )); then
		RET=0
	else
		RET=1
		printf "Isochron TX timestamp %s sent outside expected window (%s - %s)\n" \
			$(ns_to_time $tx_tstamp) \
			$(ns_to_time $window_start) \
			$(ns_to_time $window_end)
	fi

	log_test "${test_name}"

	rm ${isochron_dat} 2> /dev/null

	tc qdisc del dev $h1 root
}

out_of_band()
{
	:
}

cycle_extension()
{
	:
}

h1_create()
{
	simple_if_init $h1 192.0.2.1/24
}

h1_destroy()
{
	simple_if_fini $h1 192.0.2.1/24
}

h2_create()
{
	simple_if_init $h2 192.0.2.2/24
}

h2_destroy()
{
	simple_if_fini $h2 192.0.2.2/24
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	h2=${NETIFS[p2]}

	vrf_prepare

	h1_create
	h2_create
}

cleanup()
{
	pre_cleanup

	isochron_recv_stop

	h2_destroy
	h1_destroy

	vrf_cleanup
}

trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit $EXIT_STATUS
