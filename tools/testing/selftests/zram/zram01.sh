#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2015 Oracle and/or its affiliates. All Rights Reserved.
#
# Test creates several zram devices with different filesystems on them.
# It fills each device with zeros and checks that compression works.
#
# Author: Alexey Kodanev <alexey.kodanev@oracle.com>
# Modified: Naresh Kamboju <naresh.kamboju@linaro.org>

TCID="zram01"
ERR_CODE=0

. ./zram_lib.sh

# Test will create the following number of zram devices:
dev_num=1
# This is a list of parameters for zram devices.
# Number of items must be equal to 'dev_num' parameter.
zram_max_streams="2"

# The zram sysfs node 'disksize' value can be either in bytes,
# or you can use mem suffixes. But in some old kernels, mem
# suffixes are not supported, for example, in RHEL6.6GA's kernel
# layer, it uses strict_strtoull() to parse disksize which does
# not support mem suffixes, in some newer kernels, they use
# memparse() which supports mem suffixes. So here we just use
# bytes to make sure everything works correctly.
zram_sizes="2097152" # 2MB
zram_mem_limits="2M"
zram_filesystems="ext4"
zram_algs="lzo"

zram_fill_fs()
{
	local page_size=$(getconf PAGE_SIZE)
	for i in $(seq $dev_start $dev_end); do
		echo "fill zram$i..."
		local b=0
		while [ true ]; do
			# If we fill with all zeros, every page hits
			# the same-page detection and never makes it
			# to compressed backing.  Filling the first 1K
			# of the page with (likely lowly compressible)
			# random data ensures we hit the compression
			# paths, but the highly compressible rest of
			# the page also ensures we get a sufficiently
			# high ratio to assert on below.
			local input_file='/dev/zero'
			if [ $(( (b * 1024) % page_size )) -eq 0 ]; then
			    input_file='/dev/urandom'
			fi
			dd conv=notrunc if=${input_file} of=zram${i}/file \
				oflag=append count=1 bs=1024 status=none \
				> /dev/null 2>&1 || break
			b=$(($b + 1))
		done
		echo "zram$i was filled with '$b' KB"
		sync

		local mem_used_total=`awk '{print $3}' "/sys/block/zram$i/mm_stat"`
		local v=$((100 * 1024 * $b / $mem_used_total))
		if [ "$v" -lt 100 ]; then
			 echo "FAIL compression ratio: 0.$v:1"
			 ERR_CODE=-1
			 return
		fi

		echo "zram compression ratio: $(echo "scale=2; $v / 100 " | bc):1: OK"
	done
}

check_prereqs
zram_load
zram_max_streams
zram_compress_alg
zram_set_disksizes
zram_set_memlimit
zram_makefs
zram_mount

zram_fill_fs
zram_cleanup

if [ $ERR_CODE -ne 0 ]; then
	echo "$TCID : [FAIL]"
else
	echo "$TCID : [PASS]"
fi
