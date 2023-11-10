#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This script extracts the ELF formatted livedump from block device with correct size.
# Usage: livedump_extract block_device output_file
#
# Author: Lukas Hruska <lhruska@suse.cz>
#
# This file has been put into the public domain.
# You can do whatever you want with this file.
#
device=$1
output=$2

head -c 4096 $device > /tmp/livedump_hdr
size=$(readelf -l /tmp/livedump_hdr | tail -2 | tr '\n' ' ' | tr -s ' ' \
	| cut -d ' ' -f 5,6 | xargs printf "%d + %d" | xargs expr)
size=$(expr $size / 4096)
dd if=$device of=$output count=$size bs=4096 status=progress
