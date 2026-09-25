#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# lazy symbol loading with split debuginfo

# Build a stripped binary whose symbols live in a --only-keep-debug file,
# where a function's section is NOBITS (with a different sh_offset) and has
# no PT_LOAD in the runtime ELF. Lazy and eager loading must then both fall
# back to the runtime section header and agree on the symbol addresses.

set -e

err=0
tmpdir=$(mktemp -d /tmp/__perf_test.lazy_split_debug.XXXXX)

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

skip() {
	echo "Lazy-load split debuginfo [Skipped: $1]"
	cleanup
	exit 2
}

if ! perf check feature -q libelf; then
	skip "no libelf support"
fi

for tool in cc objcopy strip readelf dd; do
	if ! command -v "${tool}" > /dev/null; then
		skip "${tool} not found"
	fi
done

cat > "${tmpdir}/prog.c" << EOF
__attribute__((section("splittext"), noinline, used))
int split_func(int x)
{
	return x * 3 + 1;
}

int main(int argc, char **argv)
{
	(void)argv;
	return split_func(argc);
}
EOF

prog="${tmpdir}/prog"
if ! cc -O1 -g -o "${prog}" "${tmpdir}/prog.c" \
	-Wl,--section-start=splittext=0x800000 2> /dev/null; then
	skip "cannot build test program"
fi
objcopy --only-keep-debug "${prog}" "${prog}.debug"
strip -s "${prog}"
objcopy --add-gnu-debuglink="${prog}.debug" "${prog}"

# The debug file must keep splittext as NOBITS at a stale offset.
debug_sec=$(readelf -SW "${prog}.debug" | grep ' splittext ' || true)
run_sec=$(readelf -SW "${prog}" | grep ' splittext ' || true)
if ! echo "${debug_sec}" | grep -q NOBITS; then
	skip "splittext is not NOBITS in the debug file"
fi
debug_off=$(echo "${debug_sec}" | sed 's/.*splittext *//' | awk '{print $3}')
run_off=$(echo "${run_sec}" | sed 's/.*splittext *//' | awk '{print $3}')
if [ -z "${run_off}" ] || [ "${debug_off}" = "${run_off}" ]; then
	skip "splittext offsets do not differ"
fi

# Drop the PT_LOAD covering splittext so program header lookup fails.
phoff=$(readelf -hW "${prog}" | awk '/Start of program headers/ {print $5}')
phentsize=$(readelf -hW "${prog}" | awk '/Size of program headers/ {print $5}')
idx=$(readelf -lW "${prog}" | awk '
	/^Program Headers:/ { in_ph = 1; next }
	in_ph && /^  Type/ { next }
	in_ph && /^ *$/ { exit }
	in_ph && /^  [A-Z]/ {
		if ($1 == "LOAD" && $3 ~ /^0x0*800000$/) { print n; exit }
		n++
	}')
if [ -z "${phoff}" ] || [ -z "${phentsize}" ] || [ -z "${idx}" ]; then
	skip "no PT_LOAD for splittext"
fi
dd if=/dev/zero of="${prog}" bs=1 seek=$((phoff + idx * phentsize)) count=4 \
	conv=notrunc 2> /dev/null
if readelf -lW "${prog}" 2> /dev/null | grep -q 'LOAD .*0x0*800000 '; then
	echo "Lazy-load split debuginfo [Failed to drop PT_LOAD]"
	err=1
fi

if [ "${err}" -eq 0 ]; then
	export PERF_BUILDID_DIR="${tmpdir}/buildid"
	if ! perf test --dso "${prog}" "Lazy and eager symbol parity" 2>&1 | \
		grep -q ': Ok$'; then
		perf test --dso "${prog}" -vv "Lazy and eager symbol parity" 2>&1 | \
			grep -E 'symbols$|mismatch|no symbols|no lazy' || true
		echo "Lazy-load split debuginfo [Failed parity]"
		err=1
	else
		echo "Lazy-load split debuginfo [Success]"
	fi
fi

cleanup
exit ${err}
