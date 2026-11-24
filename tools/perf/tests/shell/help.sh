#!/bin/bash
# perf help tests
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
out=$(mktemp /tmp/__perf_test_help.out.XXXXX)
exec_path=

cleanup() {
	rm -f "${out}"
	[ -z "${exec_path}" ] || rm -rf "${exec_path}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_help_commands() {
	echo "perf help commands test"
	if ! perf help > "${out}" 2>&1; then
		echo "perf help commands test [Failed: perf help failed]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "The most commonly used perf commands are" "${out}"; then
		echo "perf help commands test [Failed missing command list]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "See 'perf help COMMAND'" "${out}"; then
		echo "perf help commands test [Failed missing help hint]"
		err=1
		return
	fi
	echo "perf help commands test [Success]"
}

test_help_all() {
	echo "perf help -a test"
	# "available perf commands in" is printed only when the exec path holds
	# perf-* commands, which a build tree does not have. Give help -a a
	# temporary exec path with a dummy command to look for.
	exec_path=$(mktemp -d /tmp/__perf_test_help_exec.XXXXXX)
	printf '#!/bin/sh\n' > "${exec_path}/perf-dummy"
	chmod +x "${exec_path}/perf-dummy"
	if ! PERF_EXEC_PATH="${exec_path}" perf help -a > "${out}" 2>&1; then
		echo "perf help -a test [Failed: perf help -a failed]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "available perf commands in" "${out}"; then
		echo "perf help -a test [Failed missing command directory]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "  dummy" "${out}"; then
		echo "perf help -a test [Failed missing command from exec path]"
		cat "${out}"
		err=1
		return
	fi
	echo "perf help -a test [Success]"
}

test_help_commands
test_help_all

cleanup
exit $err
