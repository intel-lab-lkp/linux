# SPDX-License-Identifier: GPL-2.0-or-later
set -e

source $(dirname -- "${BASH_SOURCE[0]}")/lib.sh

# List of devices which have a VFIO selftest driver
DEVICES=(
	"8086:0b25" # Intel Data Streaming Accelerator
	"8086:0cf8" # Intel CBDMA
)

function print_supported_devices() {
	local vendor_device_id
	local id

	for vendor_device_id in "${DEVICES[@]}"; do
		read -r id <<< "${vendor_device_id}"
		lspci -D -d "${id}" | awk '{ print $1 }'
	done
}

function pick_device() {
	local bdf

	while read -r bdf; do
		if [ -n "${bdf}" ]; then
			if [ -d "${DEVICES_DIR}/${bdf}" ]; then
				echo "${bdf} has already been set up, exiting." >&2
				exit 0
			fi
			echo "${bdf}"
			return 0
		fi
	done <<< "$(print_supported_devices)"

	echo "No available supported devices found on the system." >&2
	exit 1
}

function usage() {
	echo "usage: $0 [-l] [-d <segment:bus:device.function>]" >&2
	echo "" >&2
	echo "  -l  List segment:bus:device.function numbers of supported devices." >&2
	echo "  -d  segment:bus:device.function to set up." >&2
	echo "      If -d is not specified, a device will be automatically picked." >&2
}

function main() {
	local device_bdf
	local device_dir
	local numvfs
	local driver
	local bdf_list=()

	while getopts "ld:" opt; do
		case ${opt} in
			l)
			 	echo "Supported devices: "
				print_supported_devices
				exit 0
				;;
			d) bdf_list+=("${OPTARG}") ;;
			*) usage; exit 1 ;;
		esac
	done

	if [ ${#bdf_list[@]} -eq 0 ]; then
		bdf_list=($(pick_device))
	fi

	for device_bdf in "${bdf_list[@]}"; do
		test -d /sys/bus/pci/devices/${device_bdf}

		device_dir=${DEVICES_DIR}/${device_bdf}
		if [ -d "${device_dir}" ]; then
			echo "${device_bdf} has already been set up, exiting."
			exit 0
		fi

		mkdir -p ${device_dir}

		numvfs=$(get_sriov_numvfs ${device_bdf})
		if [ "${numvfs}" ]; then
			set_sriov_numvfs ${device_bdf} 0
			echo ${numvfs} > ${device_dir}/sriov_numvfs
		fi

		driver=$(get_driver ${device_bdf})
		if [ "${driver}" ]; then
			unbind ${device_bdf} ${driver}
			echo ${driver} > ${device_dir}/driver
		fi

		set_driver_override ${device_bdf} vfio-pci
		touch ${device_dir}/driver_override

		bind ${device_bdf} vfio-pci
		touch ${device_dir}/vfio-pci
		echo "Successfully set up ${device_bdf}"
	done
}

main "$@"
