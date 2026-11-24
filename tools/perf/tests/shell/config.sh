#!/bin/bash
# perf config tests
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
tmpdir=$(mktemp -d /tmp/__perf_test_config.XXXXXX)
out="${tmpdir}/out"

# perf test exports PERF_CONFIG=/dev/null to the shell tests, use a
# private HOME instead so the file written here is the one being read.
unset PERF_CONFIG
export HOME="${tmpdir}"

cleanup() {
	rm -rf "${tmpdir}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_config_empty() {
	echo "Empty config test"
	if ! perf config --user --list > "${out}" 2>&1; then
		echo "Empty config [Failed: perf config --user --list failed]"
		cat "${out}"
		err=1
		return
	fi
	if [ -s "${out}" ]; then
		echo "Empty config [Failed: unexpected output]"
		cat "${out}"
		err=1
		return
	fi
	echo "Empty config [Success]"
}

test_config_set_get() {
	echo "Config set and get test"
	if ! perf config report.show-nr-samples=true call-graph.print-limit=5; then
		echo "Config set [Failed: perf config failed]"
		err=1
		return
	fi
	if [ ! -s "${HOME}/.perfconfig" ]; then
		echo "Config set [Failed: no config file]"
		err=1
		return
	fi

	if ! perf config --user report.show-nr-samples | grep -qx "report.show-nr-samples=true"; then
		echo "Config get [Failed: wrong report.show-nr-samples]"
		err=1
		return
	fi
	if ! perf config --user call-graph.print-limit | grep -qx "call-graph.print-limit=5"; then
		echo "Config get [Failed: wrong call-graph.print-limit]"
		err=1
		return
	fi

	if ! perf config --user --list > "${out}" 2>&1 ||
	   ! grep -qx "report.show-nr-samples=true" "${out}" ||
	   ! grep -qx "call-graph.print-limit=5" "${out}"; then
		echo "Config list [Failed: missing variables]"
		cat "${out}"
		err=1
		return
	fi
	if ! perf config --list > "${out}" 2>&1 ||
	   ! grep -qx "report.show-nr-samples=true" "${out}"; then
		echo "Config list [Failed: missing user variable in the merged view]"
		cat "${out}"
		err=1
		return
	fi

	if ! perf config report.show-nr-samples=false; then
		echo "Config update [Failed: perf config failed]"
		err=1
		return
	fi
	if ! perf config report.show-nr-samples | grep -qx "report.show-nr-samples=false"; then
		echo "Config update [Failed: value not updated]"
		err=1
		return
	fi
	echo "Config set and get test [Success]"
}

test_config_errors() {
	echo "Config error test"
	if perf config --user --system --list > "${out}" 2>&1; then
		echo "Config error [Failed: --user --system succeeded]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "only one config file at a time" "${out}"; then
		echo "Config error [Failed: missing file option error]"
		cat "${out}"
		err=1
		return
	fi

	if perf config nosection > "${out}" 2>&1; then
		echo "Config error [Failed: invalid variable succeeded]"
		cat "${out}"
		err=1
		return
	fi
	if ! grep -q "does not contain a section name" "${out}"; then
		echo "Config error [Failed: missing invalid variable error]"
		cat "${out}"
		err=1
		return
	fi
	echo "Config error test [Success]"
}

test_config_empty
test_config_set_get
test_config_errors

cleanup
exit $err
