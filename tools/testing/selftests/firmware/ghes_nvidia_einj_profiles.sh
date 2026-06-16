#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

# Run both architecture profiles on every platform; firmware silently ignores
# selectors it does not support, so a timeout just means "not this platform".
EINJ_PROFILE_NAMES="cmet_dump_status_grace cmet_dump_status_vera"

einj_list_profiles()
{
	printf '%s\n' $EINJ_PROFILE_NAMES
}

einj_select_profile()
{
	local profile=$1

	case "$profile" in
	cmet_dump_status_grace)
		# Grace CMET dump/status: informational sample, selector 3.
		EINJ_PROFILE_ERROR_TYPE=0x80000010
		EINJ_PROFILE_VENDOR_FLAGS=1
		EINJ_PROFILE_PARAM1=3
		EINJ_PROFILE_PARAM2=0
		EINJ_PROFILE_PARAM3=0
		EINJ_PROFILE_PARAM4=0
		EINJ_PROFILE_BANNER='NVIDIA Grace CPER section'
		;;
	cmet_dump_status_vera)
		# Vera CMET-NULL dump/status: informational sample, selector 0.
		EINJ_PROFILE_ERROR_TYPE=0x80000010
		EINJ_PROFILE_VENDOR_FLAGS=1
		EINJ_PROFILE_PARAM1=0
		EINJ_PROFILE_PARAM2=0
		EINJ_PROFILE_PARAM3=0
		EINJ_PROFILE_PARAM4=0
		EINJ_PROFILE_BANNER='NVIDIA Vera CPER section'
		;;
	*)
		return 1
		;;
	esac

	return 0
}
