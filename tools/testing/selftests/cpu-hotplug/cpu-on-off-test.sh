#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

SYSFS=
# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
retval=0

prerequisite()
{
	msg="skip all tests:"

	if [ $(id -u) -ne 0 ]; then
		printf "%s must be run as root\n" "$msg" >&2
		exit $ksft_skip
	fi

	taskset -p 01 $$

	SYSFS=`mount -t sysfs | head -1 | awk '{ print $3 }'`

	if [ ! -d "$SYSFS" ]; then
		printf "%s sysfs is not mounted\n" "$msg" >&2
		exit $ksft_skip
	fi

	if ! ls $SYSFS/devices/system/cpu/cpu* > /dev/null 2>&1; then
		printf "%s cpu hotplug is not supported\n" "$msg" >&2
		exit $ksft_skip
	fi

	printf "CPU online/offline summary:\n"
	online_cpus=`cat $SYSFS/devices/system/cpu/online`
	online_max=${online_cpus##*-}

	if [ "$online_cpus" = "$online_max" ]; then
		printf "%s since there is only one cpu: %s\n" "$msg" "$online_cpus"
		exit $ksft_skip
	fi

	present_cpus=`cat $SYSFS/devices/system/cpu/present`
	present_max=${present_cpus##*-}
	printf "present_cpus = %s,  present_max = %s\n" "$present_cpus" "$present_max"

	printf "\t Cpus in online state: %s\n" "$online_cpus"

	offline_cpus=`cat $SYSFS/devices/system/cpu/offline`
	if [ "a$offline_cpus" = "a" ]; then
		offline_cpus=0
	else
		offline_max=${offline_cpus##*-}
	fi
	printf "\t Cpus in offline state: %s\n" "$offline_cpus"
}

#
# list all hot-pluggable CPUs
#
hotpluggable_cpus()
{
	local state=${1:-.\*}

	for cpu in $SYSFS/devices/system/cpu/cpu*; do
		if [ -f $cpu/online ] && grep -q $state $cpu/online; then
			printf "%s\n" "${cpu##/*/cpu}"
		fi
	done
}

hotpluggable_offline_cpus()
{
	hotpluggable_cpus 0
}

hotpluggable_online_cpus()
{
	hotpluggable_cpus 1
}

cpu_is_online()
{
	grep -q 1 $SYSFS/devices/system/cpu/cpu$1/online
}

cpu_is_offline()
{
	grep -q 0 $SYSFS/devices/system/cpu/cpu$1/online
}

online_cpu()
{
	echo 1 > $SYSFS/devices/system/cpu/cpu$1/online
}

offline_cpu()
{
	echo 0 > $SYSFS/devices/system/cpu/cpu$1/online
}

online_cpu_expect_success()
{
	FUNC="online_cpu_expect_success()"
	local cpu=$1

	if ! online_cpu $cpu; then
		printf "%s %s: unexpected fail\n" "$FUNC" "$cpu" >&2
		retval=1
	elif ! cpu_is_online $cpu; then
		printf "%s %s: unexpected offline\n" "$FUNC" "$cpu" >&2
		retval=1
	fi
}

online_cpu_expect_fail()
{

	FUNC="online_cpu_expect_fail()"
	local cpu=$1

	if online_cpu $cpu 2> /dev/null; then
		printf "%s %s: unexpected success\n" "$FUNC" "$cpu" >&2
		retval=1
	elif ! cpu_is_offline $cpu; then
		printf "%s %s: unexpected online\n" "$FUNC" "$cpu" >&2
		retval=1
	fi
}

offline_cpu_expect_success()
{

	FUNC="offline_cpu_expect_success()"
	local cpu=$1

	if ! offline_cpu $cpu; then
		printf "%s %s: unexpected fail\n" "$FUNC" "$cpu" >&2
		retval=1
	elif ! cpu_is_offline $cpu; then
		printf "%s %s: unexpected offline\n" "$FUNC" "$cpu" >&2
		retval=1
	fi
}

offline_cpu_expect_fail()
{
	local cpu=$1
	FUNC="offline_cpu_expect_fail()"

	if offline_cpu $cpu 2> /dev/null; then
		printf "%s %s: unexpected success\n" "$FUNC" "$cpu" >&2
		retval=1
	elif ! cpu_is_online $cpu; then
		printf "%s %s: unexpected offline\n" "$FUNC" "$cpu" >&2
		retval=1
	fi
}

online_all_hot_pluggable_cpus()
{
	for cpu in `hotpluggable_offline_cpus`; do
		online_cpu_expect_success $cpu
	done
}

offline_all_hot_pluggable_cpus()
{
	local reserve_cpu=$online_max
	for cpu in `hotpluggable_online_cpus`; do
		# Reserve one cpu oneline at least.
		if [ $cpu -eq $reserve_cpu ];then
			continue
		fi
		offline_cpu_expect_success $cpu
	done
}

allcpus=0
online_cpus=0
online_max=0
offline_cpus=0
offline_max=0
present_cpus=0
present_max=0

while getopts ah opt; do
	case $opt in
	a)
		allcpus=1
		;;
	h)
		printf "Usage %s [ -a ]\n" "$0"
		printf "\t default offline one cpu\n"
		printf "\t run with -a option to offline all cpus\n"
		exit
		;;
	esac
done

prerequisite

#
# Safe test (default) - offline and online one cpu
#
if [ "$allcpus" -eq 0 ]; then
	printf "Limited scope test: one hotplug cpu\n"
	printf "\t (leaves cpu in the original state):\n"
	printf "\t online to offline to online: cpu %s\n" "$online_max"
	offline_cpu_expect_success $online_max
	online_cpu_expect_success $online_max

	if [ "$offline_cpus" -gt 0 ]; then
		printf "\t online to offline to online: cpu %s\n" "$present_max"
		online_cpu_expect_success $present_max
		offline_cpu_expect_success $present_max
		online_cpu $present_max
	fi
	exit $retval
else
	printf "Full scope test: all hotplug cpus\n"
	printf "\t online all offline cpus\n"
	printf "\t offline all online cpus\n"
	printf "\t online all offline cpus\n"
fi

online_all_hot_pluggable_cpus

offline_all_hot_pluggable_cpus

online_all_hot_pluggable_cpus

exit $retval
