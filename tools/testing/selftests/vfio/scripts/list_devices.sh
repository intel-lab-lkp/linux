#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

# List of devices which have a VFIO selftest driver
readonly DEVICES=(
	"8086:0b25,Intel SPR DSA"
	"8086:11fb,Intel GNR-D DSA"
	"8086:1212,Intel DR DSA"
	"8086:0cf8,Intel CBDMA"
)

QUIET=false
while getopts "q" opt; do
	case $opt in
		q) QUIET=true ;;
		\?) echo "Usage: $0 [-q]" >&2; exit 1 ;;
	esac
done

# Print the segment:bus:device.function numbers of PCI devices that can be used
# to run VFIO selftests.
function main() {
	local id_name
	local name
	local bdfs
	local bdf
	local id

	for id_name in "${DEVICES[@]}"; do
		IFS=',' read -r id name <<< "$id_name"
		bdfs=$(lspci -D -d "${id}" | awk '{print $1}')

		[[ -z $bdfs ]] && continue

		if [ "$QUIET" = true ]; then
			echo "${bdfs}"
			continue
		fi

		while read -r bdf; do
			echo "${bdf} - ${name} (${id})"
		done <<< "$bdfs"
	done
}

main
