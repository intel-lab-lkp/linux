#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
TCID="zram.sh"

. ./zram_lib.sh

usage()
{
	echo "Usage: $0 [-a <algorithm>]"
	echo "  -a <algorithm>  compression algorithm to test (default: lzo)"
	echo "                  must be listed in /sys/block/zramN/comp_algorithm"
	echo "  -h              show this help message"
}

ZRAM_ALG="lzo"

while getopts "a:h" opt; do
	case "$opt" in
	a) ZRAM_ALG="$OPTARG" ;;
	h) usage; exit 0 ;;
	*) echo "Unknown option: $opt" >&2; usage; exit 1 ;;
	esac
done
shift $((OPTIND - 1))

run_zram () {
echo "--------------------"
echo "running zram tests"
echo "--------------------"
./zram01.sh "$ZRAM_ALG"
echo ""
./zram02.sh
}

check_prereqs

run_zram
