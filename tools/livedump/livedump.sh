#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This is a wrapper for livedump's sysfs to make a complete memdump.
# Usage: livedump block_device
#
# Author: Lukas Hruska <lhruska@suse.cz>
#
# This file has been put into the public domain.
# You can do whatever you want with this file.
#

if [ $# -ne 1 ]; then
	>&2 echo "Usage: livedump block_device"
	>&2 echo "Not enough arugments"
	exit 1
fi

DEV=$1

write_and_check() {
	NAME=$1
	VAL=$2
	PATH=$3

	echo -n "$NAME: "
	echo $VAL > $PATH
	if [ $? -ne 0 ]; then
		exit 1
	fi
	echo "OK"
}

CUR_STATE=`head -n 1 /sys/kernel/livedump/state`
if [ $CUR_STATE -ne 0 ] && [ $CUR_STATE -ne 5 ]; then
	write_and_check "reset" 5 /sys/kernel/livedump/state
fi

write_and_check "device" $DEV /sys/kernel/livedump/output
write_and_check "init" 1 /sys/kernel/livedump/state
write_and_check "start" 2 /sys/kernel/livedump/state
write_and_check "sweep" 3 /sys/kernel/livedump/state
write_and_check "finish" 4 /sys/kernel/livedump/state
write_and_check "uninit" 5 /sys/kernel/livedump/state
