#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

readonly KSFT_SKIP=4
readonly MODULE_NAME="arm_cpumod"
readonly CPU_ID="${ARM_CPUMOD_CPU:-0}"
readonly MODULE_PATH="${ARM_CPUMOD_KO:-}"
readonly ENABLE_WRITES="${ARM_CPUMOD_ENABLE_WRITES:-0}"
readonly ENABLE_INVALID_WRITES="${ARM_CPUMOD_ENABLE_INVALID_WRITES:-0}"
readonly ALL_CPUS="${ARM_CPUMOD_ALL_CPUS:-0}"
readonly EXPECT_PROFILE="${ARM_CPUMOD_EXPECT_PROFILE:-auto}"
readonly CPU_ONLINE_PATH="/sys/devices/system/cpu/online"
readonly COMMON_ATTRS=(affected_cpus pf_dis pf_mode)
readonly COMMON_RW_ATTRS=(pf_dis pf_mode)
readonly GRACE_ATTRS=(cbusy_filter_threshold cbusy_filter_window cmc_min_ways)
readonly GRACE_RW_ATTRS=(cbusy_filter_threshold cbusy_filter_window cmc_min_ways)
readonly VERA_ATTRS=(l2spr_cmc_max_ways)
readonly VERA_RW_ATTRS=(l2spr_cmc_max_ways)

loaded_by_test=0
reused_loaded_module=0
detected_profile=""
current_cpu="${CPU_ID}"
verified_cpus=0
CPU_LIST=()

log()
{
	echo "${MODULE_NAME} selftest: $*"
}

skip()
{
	log "$*"
	exit "${KSFT_SKIP}"
}

fail()
{
	log "$*"
	exit 1
}

cpu_dir()
{
	local cpu="$1"

	printf '/sys/devices/system/cpu/cpu%s' "${cpu}"
}

cpu_is_online()
{
	local cpu="$1"
	local dir="$(cpu_dir "${cpu}")"
	local online="${dir}/online"
	local value

	if [ ! -e "${online}" ]; then
		[ -d "${dir}" ] || return 1
		return 0
	fi

	[ -r "${online}" ] || return 1
	value=$(cat "${online}") || return 1
	[ "${value}" = "1" ]
}

cpumod_dir()
{
	local cpu="$1"

	printf '%s/cpumod' "$(cpu_dir "${cpu}")"
}

current_cpumod_dir()
{
	cpumod_dir "${current_cpu}"
}

cleanup()
{
	if [ "${loaded_by_test}" -eq 1 ]; then
		rmmod "${MODULE_NAME}" >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || skip "missing required command: $1"
}

module_loaded()
{
	grep -q "^${MODULE_NAME} " /proc/modules
}

wait_for_path_state()
{
	local expect_present="$1"
	local target="$2"
	local i

	for i in $(seq 1 50); do
		if [ "${expect_present}" = "present" ]; then
			[ -e "${target}" ] && return 0
		else
			[ ! -e "${target}" ] && return 0
		fi
		sleep 0.1
	done

	return 1
}

read_attr()
{
	local attr="$1"
	local file="$(current_cpumod_dir)/${attr}"
	local value

	value=$(cat "${file}") || fail "failed to read ${file}"
	printf '%s' "${value}"
}

attr_exists()
{
	local attr="$1"

	[ -f "$(current_cpumod_dir)/${attr}" ]
}

require_attr_present()
{
	local attr="$1"

	attr_exists "${attr}" || fail "missing sysfs attribute $(current_cpumod_dir)/${attr}"
}

require_attr_absent()
{
	local attr="$1"

	attr_exists "${attr}" && fail "unexpected sysfs attribute $(current_cpumod_dir)/${attr}"
}

check_decimal_attr()
{
	local attr="$1"
	local value

	value=$(read_attr "${attr}")
	case "${value}" in
	''|*[!0-9]*)
		fail "${attr} returned non-decimal value: ${value}"
		;;
	esac
}

expand_online_cpus()
{
	local cpu_list="$1"
	local part
	local start
	local start_num
	local end
	local end_num
	local cpu
	local parts

	CPU_LIST=()
	IFS=, read -ra parts <<< "${cpu_list}"
	for part in "${parts[@]}"; do
		part="${part//[[:space:]]/}"
		[ -n "${part}" ] || fail "empty CPU range in ${cpu_list}"

		if [[ "${part}" == *-* ]]; then
			start="${part%-*}"
			end="${part#*-}"
			[[ "${start}" =~ ^[0-9]+$ ]] || fail "invalid CPU range start: ${part}"
			[[ "${end}" =~ ^[0-9]+$ ]] || fail "invalid CPU range end: ${part}"
			start_num=$((10#${start}))
			end_num=$((10#${end}))
			(( start_num <= end_num )) || fail "invalid descending CPU range: ${part}"

			for ((cpu = start_num; cpu <= end_num; cpu++)); do
				CPU_LIST+=("${cpu}")
			done
		else
			[[ "${part}" =~ ^[0-9]+$ ]] || fail "invalid CPU entry: ${part}"
			CPU_LIST+=("$((10#${part}))")
		fi
	done

	[ "${#CPU_LIST[@]}" -gt 0 ] || fail "no online CPUs parsed from ${cpu_list}"
}

select_cpus()
{
	if [ "${ALL_CPUS}" = "1" ]; then
		[ -r "${CPU_ONLINE_PATH}" ] || skip "missing ${CPU_ONLINE_PATH}"
		expand_online_cpus "$(cat "${CPU_ONLINE_PATH}")"
	else
		cpu_is_online "${CPU_ID}" || \
			skip "cpu${CPU_ID} is offline or its online state is unreadable"
		CPU_LIST=("${CPU_ID}")
	fi
}

detect_profile()
{
	local has_grace
	local has_vera
	local attr
	local i
	local dir="$(current_cpumod_dir)"

	for i in $(seq 1 50); do
		has_grace=0
		has_vera=0

		for attr in "${GRACE_ATTRS[@]}"; do
			if attr_exists "${attr}"; then
				has_grace=1
				break
			fi
		done

		for attr in "${VERA_ATTRS[@]}"; do
			if attr_exists "${attr}"; then
				has_vera=1
				break
			fi
		done

		if [ "${has_grace}" -eq 1 ] && [ "${has_vera}" -eq 1 ]; then
			fail "mixed Grace/Vera-specific attributes under ${dir}"
		fi

		if [ "${has_grace}" -eq 1 ]; then
			detected_profile="grace"
			break
		fi

		if [ "${has_vera}" -eq 1 ]; then
			detected_profile="vera"
			break
		fi

		sleep 0.1
	done

	[ -n "${detected_profile}" ] || \
		fail "unable to detect Grace or Vera sysfs layout under ${dir}"

	if [ "${EXPECT_PROFILE}" != "auto" ] && [ "${EXPECT_PROFILE}" != "${detected_profile}" ]; then
		fail "expected ${EXPECT_PROFILE} sysfs layout, detected ${detected_profile}"
	fi
}

check_requirements()
{
	case "${CPU_ID}" in
	''|*[!0-9]*)
		fail "ARM_CPUMOD_CPU must be a decimal CPU index"
		;;
	esac

	case "${EXPECT_PROFILE}" in
	auto|grace|vera)
		;;
	*)
		fail "ARM_CPUMOD_EXPECT_PROFILE must be auto, grace, or vera"
		;;
	esac

	case "${ENABLE_WRITES}" in
	0|1)
		;;
	*)
		fail "ARM_CPUMOD_ENABLE_WRITES must be 0 or 1"
		;;
	esac

	case "${ENABLE_INVALID_WRITES}" in
	0|1)
		;;
	*)
		fail "ARM_CPUMOD_ENABLE_INVALID_WRITES must be 0 or 1"
		;;
	esac

	case "${ALL_CPUS}" in
	0|1)
		;;
	*)
		fail "ARM_CPUMOD_ALL_CPUS must be 0 or 1"
		;;
	esac

	[ "$(uname -m)" = "aarch64" ] || skip "requires an arm64 host"
	[ "$(id -u)" -eq 0 ] || skip "must be run as root"
	select_cpus
	for current_cpu in "${CPU_LIST[@]}"; do
		[ -d "$(cpu_dir "${current_cpu}")" ] || skip "missing CPU directory $(cpu_dir "${current_cpu}")"
	done
	require_cmd rmmod

	if [ -n "${MODULE_PATH}" ]; then
		[ -r "${MODULE_PATH}" ] || skip "ARM_CPUMOD_KO is not readable: ${MODULE_PATH}"
		require_cmd insmod
	else
		require_cmd modprobe
		modprobe -n "${MODULE_NAME}" >/dev/null 2>&1 || \
			skip "set ARM_CPUMOD_KO or install ${MODULE_NAME} into /lib/modules"
	fi
}

load_module()
{
	if module_loaded; then
		if [ -n "${MODULE_PATH}" ]; then
			skip "${MODULE_NAME} is already loaded; unload it before using ARM_CPUMOD_KO"
		fi

		reused_loaded_module=1
		log "${MODULE_NAME} already loaded, reusing existing instance"
		return 0
	fi

	if [ -n "${MODULE_PATH}" ]; then
		insmod "${MODULE_PATH}" || fail "insmod failed for ${MODULE_PATH}"
		module_loaded || fail "${MODULE_NAME} did not appear in /proc/modules after insmod"
	else
		modprobe "${MODULE_NAME}" || fail "modprobe failed for ${MODULE_NAME}"
		module_loaded || skip "${MODULE_NAME} did not appear in /proc/modules after modprobe; built-in or non-unloadable setup is unsupported"
	fi

	loaded_by_test=1
}

check_sysfs_layout()
{
	local attr
	local expected_cpu
	local value
	local dir="$(current_cpumod_dir)"

	wait_for_path_state present "${dir}" || \
		fail "cpumod sysfs directory did not appear at ${dir}"

	for attr in "${COMMON_ATTRS[@]}"; do
		require_attr_present "${attr}"
	done

	detect_profile

	case "${detected_profile}" in
	grace)
		for attr in "${GRACE_ATTRS[@]}"; do
			require_attr_present "${attr}"
		done
		for attr in "${VERA_ATTRS[@]}"; do
			require_attr_absent "${attr}"
		done
		;;
	vera)
		for attr in "${GRACE_ATTRS[@]}"; do
			require_attr_absent "${attr}"
		done
		for attr in "${VERA_ATTRS[@]}"; do
			require_attr_present "${attr}"
		done
		;;
	esac

	expected_cpu=$((10#${current_cpu}))
	value=$(read_attr affected_cpus)
	[ "${value}" = "${expected_cpu}" ] || \
		fail "cpu${current_cpu} affected_cpus expected ${expected_cpu}, got ${value}"

	for attr in "${COMMON_RW_ATTRS[@]}"; do
		check_decimal_attr "${attr}"
	done

	case "${detected_profile}" in
	grace)
		for attr in "${GRACE_RW_ATTRS[@]}"; do
			check_decimal_attr "${attr}"
		done
		;;
	vera)
		for attr in "${VERA_RW_ATTRS[@]}"; do
			check_decimal_attr "${attr}"
		done
		;;
	esac
}

writeback_same_value()
{
	local attr="$1"
	local before
	local after

	before=$(read_attr "${attr}")
	printf '%s\n' "${before}" > "$(current_cpumod_dir)/${attr}" || \
		fail "failed to write back cpu${current_cpu}/${attr}=${before}"
	after=$(read_attr "${attr}")
	[ "${after}" = "${before}" ] || \
		fail "cpu${current_cpu}/${attr} changed across writeback: before=${before} after=${after}"
}

check_writeback_same_value()
{
	local attr

	[ "${ENABLE_WRITES}" = "1" ] || return 0

	for attr in "${COMMON_RW_ATTRS[@]}"; do
		writeback_same_value "${attr}"
	done

	case "${detected_profile}" in
	grace)
		for attr in "${GRACE_RW_ATTRS[@]}"; do
			writeback_same_value "${attr}"
		done
		;;
	vera)
		for attr in "${VERA_RW_ATTRS[@]}"; do
			writeback_same_value "${attr}"
		done
		;;
	esac
}

check_invalid_write()
{
	local attr="$1"
	local value="$2"
	local before
	local after
	local file="$(current_cpumod_dir)/${attr}"

	before=$(read_attr "${attr}")
	if printf '%s\n' "${value}" > "${file}" 2>/dev/null; then
		fail "cpu${current_cpu}/${attr} accepted invalid value ${value}"
	fi
	after=$(read_attr "${attr}")
	[ "${after}" = "${before}" ] || \
		fail "cpu${current_cpu}/${attr} changed after invalid write: before=${before} after=${after}"
}

check_invalid_writes()
{
	[ "${ENABLE_INVALID_WRITES}" = "1" ] || return 0

	check_invalid_write pf_dis 2
	check_invalid_write pf_mode 10
	check_invalid_write pf_mode 99

	case "${detected_profile}" in
	grace)
		check_invalid_write cbusy_filter_threshold 4
		check_invalid_write cbusy_filter_window 4
		check_invalid_write cmc_min_ways 8
		;;
	vera)
		check_invalid_write l2spr_cmc_max_ways 8
		;;
	esac
}

check_cpu()
{
	current_cpu="$1"
	detected_profile=""
	check_sysfs_layout
	log "cpu${current_cpu}: detected ${detected_profile} sysfs layout"
	check_writeback_same_value
	check_invalid_writes
}

unload_and_verify_cleanup()
{
	local cpu
	local dir

	if [ "${loaded_by_test}" -ne 1 ]; then
		log "module was already loaded; leaving it in place and skipping unload cleanup check"
		return 0
	fi

	rmmod "${MODULE_NAME}" || fail "rmmod failed for ${MODULE_NAME}"
	loaded_by_test=0
	for cpu in "${CPU_LIST[@]}"; do
		dir="$(cpumod_dir "${cpu}")"
		wait_for_path_state absent "${dir}" || \
			fail "${dir} still present after unload"
	done
}

check_requirements
load_module
for current_cpu in "${CPU_LIST[@]}"; do
	if [ ! -d "$(cpumod_dir "${current_cpu}")" ]; then
		cpu_is_online "${current_cpu}" || \
			fail "cpu${current_cpu} went offline or its online state became unreadable"
		log "cpu${current_cpu}: no cpumod directory; unsupported CPU profile, skipping"
		continue
	fi

	check_cpu "${current_cpu}"
	verified_cpus=$((verified_cpus + 1))
done
unload_and_verify_cleanup
[ "${verified_cpus}" -gt 0 ] || \
	skip "no selected CPU exposes a supported cpumod profile"

if [ "${reused_loaded_module}" -eq 1 ]; then
	log "PASS verified_cpus=${verified_cpus} expect_profile=${EXPECT_PROFILE} writes=${ENABLE_WRITES} invalid_writes=${ENABLE_INVALID_WRITES} all_cpus=${ALL_CPUS} (reused pre-loaded module; unload cleanup check skipped)"
else
	log "PASS verified_cpus=${verified_cpus} expect_profile=${EXPECT_PROFILE} writes=${ENABLE_WRITES} invalid_writes=${ENABLE_INVALID_WRITES} all_cpus=${ALL_CPUS}"
fi
exit 0
