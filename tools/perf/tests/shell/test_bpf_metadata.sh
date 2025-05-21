#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# BPF metadata collection test.

set -e

err=0
perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)

cleanup() {
	rm -f "${perfdata}"
	rm -f "${perfdata}".old
	trap - EXIT TERM INT
}

trap_cleanup() {
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_bpf_metadata() {
	echo "Checking BPF metadata collection"

	# This is a basic invocation of perf record
	# that invokes the perf_sample_filter BPF program.
	if ! perf record -e task-clock --filter 'ip > 0' \
			 -o "${perfdata}" sleep 1 2> /dev/null
	then
		echo "Basic BPF metadata test [Failed record]"
		err=1
		return
	fi

	# The perf_sample_filter BPF program has the following variable in it:
	#
	#   volatile const int bpf_metadata_test_value SEC(".rodata") = 42;
	#
	# This invocation looks for a PERF_RECORD_BPF_METADATA event,
	# and checks that its content includes something for the above variable.
	if ! perf script --show-bpf-events -i "${perfdata}" | awk '
		/PERF_RECORD_BPF_METADATA.*perf_sample_filter/ {
			header = 1;
		}
		/^ *entry/ {
			if (header) { header = 0; entry = 1; }
		}
		$0 !~ /^ *entry/ {
			entry = 0;
		}
		/test_value/ {
			if (entry) print $NF;
		}
	' | grep 42 > /dev/null
	then
		echo "Basic BPF metadata test [Failed invalid output]"
		err=1
		return
	fi
	echo "Basic BPF metadata test [Success]"
}

test_bpf_metadata

cleanup
exit $err
