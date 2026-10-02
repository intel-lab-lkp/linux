#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# perf script lazy symbol loading tests (exclusive)
#
# Verifies that --lazy-load-symbols matches the default eager loader for a
# controlled symbol.

mark_skip() {
	if [ "${err}" -eq 0 ]; then
		err=2
	fi
	return 0
}

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

testsym="test_loop"
perf_path=$(readlink -f "$(command -v perf)")

skip_test_missing_symbol ${testsym}

if ! perf check feature -q libelf
then
	echo "Lazy symbol loading [Skipped no libelf support]"
	exit 2
fi

err=0
temp_dir=$(mktemp -d /tmp/__perf_test.lazy_load.XXXXX)
perfdata="${temp_dir}/perf.data"
eager_out="${temp_dir}/eager.out"
lazy_out="${temp_dir}/lazy.out"
lazy_err="${temp_dir}/lazy.err"
eager_sym_out="${temp_dir}/eager.sym.out"
lazy_sym_out="${temp_dir}/lazy.sym.out"
ns_pid=

cleanup() {
	if [ -n "${ns_pid}" ]; then
		kill "${ns_pid}" 2> /dev/null || true
		wait "${ns_pid}" 2> /dev/null || true
	fi
	rm -rf "${temp_dir}"
	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_lazy_load_identical() {
	echo "Lazy-load output matches eager loader"

	if ! perf record -o "${perfdata}" -g -- perf test -w thloop 2> /dev/null
	then
		echo "Lazy-load identical [Skipped record not supported]"
		mark_skip
		return 0
	fi

	if ! perf script -i "${perfdata}" 2> /dev/null > "${eager_out}" || \
	   ! perf script -v --lazy-load-symbols -i "${perfdata}" \
		2> "${lazy_err}" > "${lazy_out}"
	then
		echo "Lazy-load identical [Failed perf script error]"
		err=1
		return
	fi
	if ! grep -q "on-demand index:" "${lazy_err}"
	then
		echo "Lazy-load identical [Failed lazy loader fell back to eager]"
		err=1
		return
	fi
	if ! grep -Fq "${perf_path}: on-demand index:" "${lazy_err}"
	then
		echo "Lazy-load identical [Failed controlled DSO has no index]"
		err=1
		return
	fi

	# The comparison is only meaningful if something actually resolved;
	# two all-[unknown] outputs would also match.
	if ! grep -q "${testsym}" "${eager_out}"
	then
		echo "Lazy-load identical [Skipped no ${testsym} resolved]"
		mark_skip
		return 0
	fi

	grep -w -o "${testsym}" "${eager_out}" > "${eager_sym_out}"
	if ! grep -w -o "${testsym}" "${lazy_out}" > "${lazy_sym_out}"
	then
		echo "Lazy-load identical [Failed no lazy ${testsym} resolved]"
		err=1
		return
	fi

	if ! cmp -s "${eager_sym_out}" "${lazy_sym_out}"
	then
		echo "Lazy-load identical [Failed ${testsym} output differs]"
		err=1
		return
	fi
	echo "Lazy-load identical [Success]"
}

# A build-id cache file is opened outside the target's mount namespace, so
# lazy name reads must open it there too.
test_lazy_load_mount_ns() {
	echo "Lazy-load build-id cache source of a process in a mount namespace"

	if [ "$(id -u)" != 0 ] || ! command -v unshare > /dev/null
	then
		echo "Lazy-load mount namespace [Skipped needs root and unshare]"
		mark_skip
		return 0
	fi

	local buildid_dir="${temp_dir}/buildid"
	local nsdata="${temp_dir}/ns.data"
	local i

	# The workload sees an empty build-id cache; perf sees the real one.
	mkdir -p "${buildid_dir}"
	unshare -m --propagation private sh -c \
		"mount -t tmpfs none '${buildid_dir}' && exec perf test -w thloop 30" &
	ns_pid=$!
	for i in $(seq 50)
	do
		if [ "$(readlink "/proc/${ns_pid}/exe" 2> /dev/null)" = "${perf_path}" ]
		then
			break
		fi
		sleep 0.1
	done
	if [ "$(readlink "/proc/${ns_pid}/exe" 2> /dev/null)" != "${perf_path}" ]
	then
		echo "Lazy-load mount namespace [Skipped cannot start workload]"
		mark_skip
		return 0
	fi

	if ! perf record -N -o "${nsdata}" -p "${ns_pid}" -- sleep 1 2> /dev/null ||
	   ! perf --buildid-dir "${buildid_dir}" buildid-cache -a "${perf_path}" \
		2> /dev/null ||
	   [ -z "$(ls -A "${buildid_dir}/.build-id" 2> /dev/null)" ]
	then
		echo "Lazy-load mount namespace [Skipped record or build-id cache failed]"
		mark_skip
		return 0
	fi
	perf --buildid-dir "${buildid_dir}" script -i "${nsdata}" 2> /dev/null \
		> "${eager_out}" || true
	perf --buildid-dir "${buildid_dir}" script -v --lazy-load-symbols \
		-i "${nsdata}" 2> "${lazy_err}" > "${lazy_out}" || true

	if ! grep -Fq "${perf_path}: on-demand index:" "${lazy_err}"
	then
		echo "Lazy-load mount namespace [Failed no index for the build-id cache source]"
		err=1
		return
	fi
	if ! grep -q "${testsym}" "${eager_out}" ||
	   ! cmp -s "${eager_out}" "${lazy_out}"
	then
		echo "Lazy-load mount namespace [Failed output differs from eager]"
		err=1
		return
	fi
	echo "Lazy-load mount namespace [Success]"
}

test_lazy_load_identical
test_lazy_load_mount_ns

cleanup
exit $err
