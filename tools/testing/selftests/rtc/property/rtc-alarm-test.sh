#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

if [ ! -f /proc/driver/rtc ]; then
	echo "SKIP: the /proc/driver/rtc is empty."
	exit 4
fi

# Check if could find alarm content in /proc/driver/rtc according to
# the rtc wakealarm is supported or not.
if [ -n "$(ls /sys/class/rtc/rtc* | grep -i wakealarm)" ]; then
	if [ -n "$(grep -i alarm /proc/driver/rtc)" ]; then
		exit 0
	else
		echo "ERROR: The alarm content is not found."
		cat /proc/driver/rtc
		exit 1
	fi
else
	if [ -n "$(grep -i alarm /proc/driver/rtc)" ]; then
		echo "ERROR: The alarm content is found."
		cat /proc/driver/rtc
		exit 1
	else
		exit 0
	fi
fi
