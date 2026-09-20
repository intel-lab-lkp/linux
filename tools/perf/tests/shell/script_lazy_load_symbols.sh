#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# perf script lazy symbol loading tests (exclusive)
#
# Verifies that --lazy-load-symbols matches the default eager loader for a
# controlled symbol, and that --max-symbol-bytes caps symbol allocations
# (emitting [unknown] plus a warning) without crashing.

mark_skip() {
	if [ "${err}" -eq 0 ]; then
		err=2
	fi
	return 0
}

if [ "${PERF_LAZY_LOAD_SYMBOLS_TEST_HELPERS:-}" = 1 ]; then
	return 0
fi

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

testsym="test_loop"
perf_path=$(readlink -f "$(command -v perf)")
lazy_index_budget=

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

cleanup() {
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

	# Record a small profile with callchains so symbol resolution runs.
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
	lazy_index_budget=$(awk -v dso="${perf_path}: on-demand index:" \
		'index($0, dso) { sub(/^.* budget=/, ""); print; exit }' \
		"${lazy_err}")
	case "${lazy_index_budget}" in
	''|*[!0-9]*)
		echo "Lazy-load identical [Failed controlled DSO has no index]"
		err=1
		return
		;;
	esac

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

test_max_symbol_bytes() {
	echo "--max-symbol-bytes budget enforcement"

	# Depends on ${perfdata} from test_lazy_load_identical.
	if [ ! -s "${perfdata}" ]
	then
		echo "--max-symbol-bytes budget [Skipped record not supported]"
		mark_skip
		return 0
	fi

	# A tiny budget forces most symbols to be dropped as [unknown],
	# with a single warning, and must not crash.
	if ! perf script --max-symbol-bytes=1K -i "${perfdata}" > /dev/null \
		2> "${temp_dir}/budget.err"
	then
		echo "--max-symbol-bytes budget [Failed nonzero exit]"
		err=1
		return
	fi
	if ! grep -q "symbol memory budget exceeded" "${temp_dir}/budget.err"
	then
		echo "--max-symbol-bytes budget [Failed missing warning]"
		err=1
		return
	fi
	if perf script --max-symbol-bytes=1Kjunk -i "${perfdata}" \
		> /dev/null 2>&1
	then
		echo "--max-symbol-bytes budget [Failed malformed size accepted]"
		err=1
		return
	fi
	if ! perf script --max-symbol-bytes=0 -i "${perfdata}" \
		> /dev/null 2>&1
	then
		echo "--max-symbol-bytes budget [Failed zero not accepted]"
		err=1
		return
	fi

	# The unlimited run logged the peak accounted bytes at the controlled
	# DSO's index reservation, before alias dedup may have released bytes.
	# Reuse that peak as the budget: deterministic index construction fits,
	# while subsequent materialization must hit the limit.
	if ! perf script -v --lazy-load-symbols \
		--max-symbol-bytes="${lazy_index_budget}B" \
		-i "${perfdata}" > /dev/null 2> "${temp_dir}/lazy-budget.err"
	then
		echo "--max-symbol-bytes lazy budget [Failed nonzero exit]"
		err=1
		return
	fi
	if ! grep -Fq "${perf_path}: on-demand index:" \
		"${temp_dir}/lazy-budget.err" ||
	   ! grep -q "symbol memory budget exceeded" "${temp_dir}/lazy-budget.err"
	then
		echo "--max-symbol-bytes lazy budget [Failed no indexed budget case]"
		err=1
		return
	fi
	warnings=$(grep -c "symbol memory budget exceeded" \
		"${temp_dir}/lazy-budget.err" || true)
	if [ "${warnings}" -ne 1 ]
	then
		echo "--max-symbol-bytes lazy budget [Failed warning count: ${warnings}]"
		err=1
		return
	fi
	echo "--max-symbol-bytes budget [Success]"
}

test_budget_truncation_range() {
	local longsym
	local first_symbol
	local trunc_source="${temp_dir}/truncation.S"
	local trunc_binary="${temp_dir}/truncation"
	local trunc_data="${temp_dir}/truncation.data"
	local trunc_out="${temp_dir}/truncation.out"
	local trunc_err="${temp_dir}/truncation.err"

	echo "--max-symbol-bytes truncation range"

	if [ "$(uname -m)" != x86_64 ]; then
		echo "--max-symbol-bytes truncation range [Skipped x86_64 only]"
		mark_skip
		return 0
	fi

	longsym=$(printf 's%.0s' {1..900})
	cat > "${trunc_source}" <<EOF
	.text
	.globl ${longsym}
	.type ${longsym}, @function
${longsym}:
	call omitted_symbol
	mov \$60, %eax
	xor %edi, %edi
	syscall

	.globl omitted_symbol
	.type omitted_symbol, @function
omitted_symbol:
	mov \$500000000, %ecx
1:
	dec %ecx
	jnz 1b
	ret
	.size omitted_symbol, .-omitted_symbol
EOF
	if ! cc -nostdlib -no-pie -Wl,--build-id=none -Wl,-e,"${longsym}" \
		-o "${trunc_binary}" "${trunc_source}"
	then
		echo "--max-symbol-bytes truncation range [Skipped compiler unsupported]"
		mark_skip
		return 0
	fi

	first_symbol=$(readelf -W -s "${trunc_binary}" |
		awk '$4 == "FUNC" && $7 != "UND" { print $8; exit }')
	if [ "${first_symbol}" != "${longsym}" ]; then
		echo "--max-symbol-bytes truncation range [Skipped unexpected symbol order]"
		mark_skip
		return 0
	fi

	if ! perf record -o "${trunc_data}" -e cycles:u -F 1000 -- \
		"${trunc_binary}" 2> /dev/null
	then
		echo "--max-symbol-bytes truncation range [Skipped record not supported]"
		mark_skip
		return 0
	fi
	if ! perf script --max-symbol-bytes=1K -i "${trunc_data}" -F ip,sym,dso \
		> "${trunc_out}" 2> "${trunc_err}"
	then
		echo "--max-symbol-bytes truncation range [Failed perf script error]"
		err=1
		return
	fi

	if ! grep -q "symbol memory budget exceeded" "${trunc_err}" ||
	   ! grep -F "${trunc_binary}" "${trunc_out}" | grep -q '\[unknown\]' ||
	   grep -Fq "${longsym}" "${trunc_out}"
	then
		echo "--max-symbol-bytes truncation range [Failed omitted range resolved]"
		err=1
		return
	fi
	echo "--max-symbol-bytes truncation range [Success]"
}

test_lazy_load_identical
test_max_symbol_bytes
test_budget_truncation_range

cleanup
exit $err
