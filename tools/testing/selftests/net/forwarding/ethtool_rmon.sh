#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ALL_TESTS="
	rmon_rx_histogram
	rmon_tx_histogram
"

NUM_NETIFS=2
source lib.sh

bucket_test()
{
	local set=$1; shift
	local bucket=$1; shift
	local len=$1; shift
	local num_rx=10000
	local num_tx=20000
	local expected=
	local before=
	local after=
	local delta=

	# Mausezahn does not include FCS bytes in its length - but the
	# histogram counters do
	len=$((len - 4))

	before=$(ethtool --json -S $h2 --groups rmon | \
		jq -r ".[0].rmon[\"${set}-pktsNtoM\"][$bucket].val")

	# Send 10k one way and 20k in the other, to detect counters
	# mapped to the wrong direction
	$MZ $h1 -q -c $num_rx -p $len -a own -b bcast -d 10us
	$MZ $h2 -q -c $num_tx -p $len -a own -b bcast -d 10us

	after=$(ethtool --json -S $h2 --groups rmon | \
		jq -r ".[0].rmon[\"${set}-pktsNtoM\"][$bucket].val")

	delta=$((after - before))

	expected=$([ $set = rx ] && echo $num_rx || echo $num_tx)

	# Allow some extra tolerance for other packets sent by the stack
	[ $delta -ge $expected ] && [ $delta -le $((expected + 100)) ]
}

rmon_histogram()
{
	local set=$1; shift
	local nbuckets=0

	RET=0

	while read -r -a bucket; do
		bucket_test $set $nbuckets ${bucket[0]}
		check_err "$?" "Verification failed for bucket ${bucket[0]}-${bucket[1]}"
		nbuckets=$((nbuckets + 1))
	done < <(ethtool --json -S $h2 --groups rmon | \
		jq -r ".[0].rmon[\"${set}-pktsNtoM\"][]|[.low, .high, .val]|@tsv" 2>/dev/null)

	if [ $nbuckets -eq 0 ]; then
		log_test_skip "$h2 does not support $set histogram counters"
		return
	fi

	log_test "$set histogram counters"
}

rmon_rx_histogram()
{
	rmon_histogram rx
}

rmon_tx_histogram()
{
	rmon_histogram tx
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	h2=${NETIFS[p2]}

	for iface in $h1 $h2; do
		ip link set dev $iface up
	done
}

cleanup()
{
	pre_cleanup

	for iface in $h2 $h1; do
		ip link set dev $iface down
	done
}

check_ethtool_counter_group_support
trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit $EXIT_STATUS
