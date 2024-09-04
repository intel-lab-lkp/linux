#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This script need root permission
uid=$(id -u)
if [ $uid -ne 0 ]; then
	echo "SKIP: Must be run as root"
	exit 4
fi

# Ensure debugfs is mounted
mount -t debugfs nodev /sys/kernel/debug 2>/dev/null
if [ ! -d "/sys/kernel/debug/irq/irqs" ]; then
	echo "SKIP: irq debugfs not found"
	exit 4
fi

# Traverse the irq debug file system directory to collect chip_name and hwirq
hwirq_list=$(for irq_file in /sys/kernel/debug/irq/irqs/*; do
	# Read chip name and hwirq from the irq_file
	chip_name=$(cat "$irq_file" | grep -m 1 'chip:' | awk '{print $2}')
	hwirq=$(cat "$irq_file" | grep -m 1 'hwirq:' | awk '{print $2}' )

	if [ -z "$chip_name" ] || [ -z "$hwirq" ]; then
		continue
	fi

	echo "$chip_name $hwirq"
done)

dup_hwirq_list=$(echo "$hwirq_list" | sort | uniq -cd)

if [ -n "$dup_hwirq_list" ]; then
	echo "ERROR: Found duplicate hwirq"
	echo "$dup_hwirq_list"
	exit 1
fi

exit 0
