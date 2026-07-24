#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

TEST_DIR=$(dirname "$0")
source "$TEST_DIR/einj_lib.sh"
source "$TEST_DIR/ghes_nvidia_einj_profiles.sh"

einj_assert_nvidia_cper_output()
{
	local profile=$1
	local output=$2

	if printf '%s\n' "$output" | grep -Fq 'Malformed NVIDIA'; then
		echo "$0: $profile produced malformed NVIDIA CPER output" >&2
		printf '%s\n' "$output" >&2
		return 1
	fi

	if printf '%s\n' "$output" | grep -Fq 'NVIDIA Grace CPER section'; then
		if ! printf '%s\n' "$output" | grep -Fq 'signature:'; then
			echo "$0: $profile Grace output missing signature line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		if ! printf '%s\n' "$output" | grep -Fq 'error_type:'; then
			echo "$0: $profile Grace output missing error_type line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		if ! printf '%s\n' "$output" | grep -Fq 'number_regs:'; then
			echo "$0: $profile Grace output missing number_regs line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		if ! printf '%s\n' "$output" | grep -Fq 'instance_base:'; then
			echo "$0: $profile Grace output missing instance_base line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		return 0
	fi

	if printf '%s\n' "$output" | grep -Fq 'NVIDIA Vera CPER section'; then
		if ! printf '%s\n' "$output" | grep -Fq 'signature:'; then
			echo "$0: $profile Vera output missing signature line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		if ! printf '%s\n' "$output" | grep -Fq 'event_type:'; then
			echo "$0: $profile Vera output missing event_type line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		if ! printf '%s\n' "$output" | grep -Fq 'event_sub_type:'; then
			echo "$0: $profile Vera output missing event_sub_type line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		if ! printf '%s\n' "$output" | grep -Fq 'event_context_count:'; then
			echo "$0: $profile Vera output missing event_context_count line" >&2
			printf '%s\n' "$output" >&2
			return 1
		fi
		return 0
	fi

	echo "$0: $profile did not emit a recognized NVIDIA CPER section" >&2
	printf '%s\n' "$output" >&2
	return 1
}

einj_run_profile()
{
	local profile=$1
	local marker
	local output

	if ! einj_select_profile "$profile"; then
		echo "$0: unknown safe NVIDIA EINJ profile: $profile" >&2
		return 1
	fi

	einj_require_writable_profile

	printf '%s: running safe sample %s\n' "$0" "$profile"
	marker=$(einj_emit_kmsg_marker "$profile")

	einj_write_value error_type "$EINJ_PROFILE_ERROR_TYPE"
	einj_write_value flags 0
	einj_write_value vendor_flags "$EINJ_PROFILE_VENDOR_FLAGS"
	einj_write_value param1 "$EINJ_PROFILE_PARAM1"
	einj_write_value param2 "$EINJ_PROFILE_PARAM2"
	einj_write_value param3 "$EINJ_PROFILE_PARAM3"
	einj_write_value param4 "$EINJ_PROFILE_PARAM4"
	einj_write_value notrigger 0
	einj_write_value error_inject 1

	output=$(einj_wait_for_dmesg_after_marker_contains "$marker" "$EINJ_PROFILE_BANNER" 10) || {
		printf '%s: %s not supported on this platform\n' "$0" "$profile"
		return "$ksft_skip"
	}

	einj_assert_nvidia_cper_output "$profile" "$output"
}

einj_cleanup()
{
	local status=$1

	if ! einj_restore_state; then
		echo "$0: failed to restore EINJ state" >&2
		[ "$status" -eq 0 ] && status=1
	fi

	exit "$status"
}

main()
{
	local profile
	local passed=0

	einj_require_root
	einj_require_debugfs
	einj_require_einj
	einj_require_vendor_einj
	einj_require_available_error_type
	einj_save_state
	trap 'einj_cleanup "$?"' EXIT

	einj_require_bound_nvidia_device

	for profile in $(einj_list_profiles); do
		einj_run_profile "$profile" && passed=$((passed + 1)) || {
			[ "$?" -eq "$ksft_skip" ] || exit 1
		}
	done

	[ "$passed" -gt 0 ] || einj_skip "no NVIDIA EINJ profiles produced output"
}

main "$@"
