#!/bin/bash
# SPDX-License-Identifier: GPL-2.0+
#
# Run hazptr torture tests and report any that fail.
#
# Usage: hazptr.sh [ --cpus N ] [ --allcpus ] [ --datestamp string ]
#
# Copyright (C) 2026 Kunwu Chan
#
# Authors: Kunwu Chan <kunwu.chan@gmail.com>

usage () {
	echo "Usage: $scriptname optional arguments:"
	echo "       --cpus N       override CPU count (default: auto-detect)"
	echo "       --allcpus      test at CPU counts 8,16,32,...,256"
	echo "       --datestamp string"
	exit 1
}

ds=`date +%Y.%m.%d-%H.%M.%S`-hazptr
scriptname="$0"

RCUTORTURE="`pwd`/tools/testing/selftests/rcutorture"; export RCUTORTURE
PATH=${RCUTORTURE}/bin:$PATH; export PATH
. functions.sh

cpus=""		# empty = let kvm.sh auto-detect
allcpus=0

while test $# -gt 0
do
	case "$1" in
	--datestamp)
		checkarg --datestamp "(relative pathname)" "$#" "$2" '^[a-zA-Z0-9._/-]*$' '^--'
		ds=$2
		shift
		;;
	--cpus)
		checkarg --cpus "(number)" "$#" "$2" '^[0-9]\+$' '^--'
		cpus="$2"
		shift
		;;
	--allcpus)
		allcpus=1
		;;
	*)
		echo Unknown argument $1
		usage
		;;
	esac
	shift
done

nerrs=0

run_kvm () {
	local ds_suffix="$1"
	local configs="$2"
	local cpu_arg="$3"
	local kconfig="$4"
	local resdir="$RCUTORTURE/res/$ds/$ds_suffix"

	mkdir -p "$resdir"

	tools/testing/selftests/rcutorture/bin/kvm.sh \
		--torture hazptr \
		--duration 5s \
		--configs "$configs" \
		${cpu_arg:+--cpus "$cpu_arg"} \
		${kconfig:+--kconfig "$kconfig"} \
		--trust-make \
		--datestamp "$ds/$ds_suffix" > "$resdir/kvm.sh.out" 2>&1
	return $?
}

check_lockdep () {
	local ds_suffix="$1" config_dir="$2" label="$3"
	local resdir="$RCUTORTURE/res/$ds/$ds_suffix"
	local console="$resdir/$config_dir/console.log"

	if ! test -f "$console"
	then
		echo "Missing console.log for $label ($ds_suffix)" > "$resdir/kvm.sh.err"
		return 1
	fi
	if grep -qE "WARNING: possible (recursive locking|circular locking dependency)" "$console"
	then
		echo "Lockdep warning in $label" > "$resdir/kvm.sh.err"
		return 1
	fi
	return 0
}

# Basic torture: all hazptr configs, CPU count auto-detected

echo "--- hazptr basic torture${cpus:+ ($cpus CPUs)} ---"
run_kvm basic "NOPREEMPT PREEMPT READERS0 READERS4" "$cpus" || nerrs=$((nerrs+1))

# LOCKDEP: verify no lockdep splats with PROVE_LOCKING + wq_churn

echo "--- hazptr lockdep + wq_churn ---"
if run_kvm lockdep-wq LOCKDEP "$cpus"
then
	check_lockdep lockdep-wq LOCKDEP "LOCKDEP wq_churn" ||
		nerrs=$((nerrs+1))
else
	nerrs=$((nerrs+1))
fi

# SLOWPATH + PROVE_LOCKING: verify forced-slowpath does not deadlock

echo "--- hazptr SLOWPATH + lockdep ---"
if run_kvm slowpath-lockdep SLOWPATH "$cpus" \
	"CONFIG_PROVE_LOCKING=y"
then
	check_lockdep slowpath-lockdep SLOWPATH \
		"SLOWPATH+PROVE_LOCKING" ||
		nerrs=$((nerrs+1))
else
	nerrs=$((nerrs+1))
fi

# CPU scaling: optionally test at multiple CPU counts

if test "$allcpus" -ne 0
then
	max_cpus=`grep '^processor' /proc/cpuinfo 2>/dev/null | wc -l`
	for n in 8 16 32 64 128 256
	do
		if test "$n" -le "$max_cpus"
		then
		echo "--- hazptr CPU=$n ---"
		run_kvm "cpu$n" "PREEMPT" "$n" "CONFIG_NR_CPUS=$n" || nerrs=$((nerrs+1))
		fi
	done
fi

# Report

if test "$nerrs" -ne 0
then
	echo "hazptr.sh: $nerrs test(s) failed"
	exit 1
fi
echo "hazptr.sh: all tests passed"
exit 0
