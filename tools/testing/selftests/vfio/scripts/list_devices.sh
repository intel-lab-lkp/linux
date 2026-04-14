#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

# List of devices which have a VFIO selftest driver
DEVICES=(
	"8086:0b25" # Intel SPR DSA
	"8086:11fb" # Intel GNR-D DSA
	"8086:1212" # Intel DR DSA
	"8086:0cf8" # Intel CBDMA
)

# Print the segment:bus:device.function numbers of PCI devices that can be used
# to run VFIO selftests.
function main() {
	local vendor_device_id

	for vendor_device_id in "${DEVICES[@]}"; do
		lspci -D -d "${vendor_device_id}" | awk '{print $1}'
	done
}

main "$@"
