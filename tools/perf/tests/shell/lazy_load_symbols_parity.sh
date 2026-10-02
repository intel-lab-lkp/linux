#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# lazy symbol loading parity with eager loading

# Build small ELF files whose symbols are hard for an index to get right, and
# check that lazy and eager loading agree on their address lookups, ranges and
# names:
#  - split debuginfo, where a function's section is NOBITS (with a different
#    sh_offset) in the debug file and has no PT_LOAD in the runtime ELF;
#  - a function containing another function symbol;
#  - a shared library with symbols that are only in .dynsym, one of them a
#    longer-named alias of a .symtab symbol;
#  - a stripped shared library whose local symbols are in .gnu_debugdata,
#    one of them an alias of a .dynsym symbol, which must load eagerly;
#  - on x86_64, an IRELATIVE PLT slot whose IFUNC loses duplicate selection
#    to an alias that loses to another one in turn.

set -e

err=0
tmpdir=$(mktemp -d /tmp/__perf_test.lazy_parity.XXXXX)

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
	echo "Lazy-load parity [Skipped: $1]"
	cleanup
	exit 2
}

if ! perf check feature -q libelf; then
	skip "no libelf support"
fi

for tool in cc objcopy strip readelf nm dd; do
	if ! command -v "${tool}" > /dev/null; then
		skip "${tool} not found"
	fi
done

# check_parity NAME FILE [eager]
# Lazy loading must build an index for FILE, or none if "eager" is given.
check_parity() {
	local out="${tmpdir}/parity.out"
	local index="on-demand index:"

	if [ "$3" = eager ]; then
		index="no lazy index was built"
	fi
	perf test --dso "$2" -vv "Lazy and eager symbol parity" > "${out}" 2>&1 || true
	if grep -q ': Ok$' "${out}" && grep -Fq "$2: ${index}" "${out}"; then
		echo "Lazy-load parity $1 [Success]"
		return
	fi
	grep -E 'symbols$|mismatch|lookup|no symbols|index' "${out}" || true
	echo "Lazy-load parity $1 [Failed]"
	err=1
}

test_split_debug() {
	local prog="${tmpdir}/split"

	cat > "${prog}.c" << EOF
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
	if ! cc -O1 -g -o "${prog}" "${prog}.c" \
		-Wl,--section-start=splittext=0x800000 2> /dev/null; then
		echo "Lazy-load parity split debuginfo [Skipped: cannot build]"
		return
	fi
	objcopy --only-keep-debug "${prog}" "${prog}.debug"
	strip -s "${prog}"
	objcopy --add-gnu-debuglink="${prog}.debug" "${prog}"

	# The debug file must keep splittext as NOBITS at a stale offset.
	local debug_sec run_sec debug_off run_off
	debug_sec=$(readelf -SW "${prog}.debug" 2> /dev/null | grep ' splittext ' || true)
	run_sec=$(readelf -SW "${prog}" | grep ' splittext ' || true)
	if ! echo "${debug_sec}" | grep -q NOBITS; then
		echo "Lazy-load parity split debuginfo [Skipped: splittext is not NOBITS]"
		return
	fi
	debug_off=$(echo "${debug_sec}" | sed 's/.*splittext *//' | awk '{print $3}')
	run_off=$(echo "${run_sec}" | sed 's/.*splittext *//' | awk '{print $3}')
	if [ -z "${run_off}" ] || [ "${debug_off}" = "${run_off}" ]; then
		echo "Lazy-load parity split debuginfo [Skipped: offsets do not differ]"
		return
	fi

	# Drop the PT_LOAD covering splittext so program header lookup fails.
	local phoff phentsize idx
	phoff=$(readelf -hW "${prog}" | awk '/Start of program headers/ {print $5}')
	phentsize=$(readelf -hW "${prog}" | awk '/Size of program headers/ {print $5}')
	idx=$(readelf -lW "${prog}" 2> /dev/null | awk '
		/^Program Headers:/ { in_ph = 1; next }
		in_ph && /^  Type/ { next }
		in_ph && /^ *$/ { exit }
		in_ph && /^  [A-Z]/ {
			if ($1 == "LOAD" && $3 ~ /^0x0*800000$/) { print n; exit }
			n++
		}')
	if [ -z "${phoff}" ] || [ -z "${phentsize}" ] || [ -z "${idx}" ]; then
		echo "Lazy-load parity split debuginfo [Skipped: no PT_LOAD for splittext]"
		return
	fi
	dd if=/dev/zero of="${prog}" bs=1 seek=$((phoff + idx * phentsize)) \
		count=4 conv=notrunc 2> /dev/null
	if readelf -lW "${prog}" 2> /dev/null | grep -q 'LOAD .*0x0*800000 '; then
		echo "Lazy-load parity split debuginfo [Failed to drop PT_LOAD]"
		err=1
		return
	fi
	check_parity "split debuginfo" "${prog}"
}

test_nested_symbol() {
	local prog="${tmpdir}/nested"

	# inner covers 4 bytes in the middle of outer, so outer's addresses
	# after inner belong to outer alone.
	cat > "${prog}.c" << EOF
__attribute__((noinline, used))
int outer(int x)
{
	int i, s = 0;

	for (i = 0; i < x; i++)
		s += i * x + (s >> 3);
	return s;
}

asm(".globl inner\n"
    ".type inner, STT_FUNC\n"
    ".set inner, outer + 8\n"
    ".size inner, 4\n");

int main(int argc, char **argv)
{
	(void)argv;
	return outer(argc * 100);
}
EOF
	if ! cc -O1 -o "${prog}" "${prog}.c" 2> /dev/null; then
		echo "Lazy-load parity nested symbol [Skipped: cannot build]"
		return
	fi
	local outer_size
	outer_size=$(nm -S "${prog}" | awk '$4 == "outer" { print $2 }')
	if [ -z "${outer_size}" ] || [ $((16#${outer_size})) -lt 16 ]; then
		echo "Lazy-load parity nested symbol [Skipped: outer is too small]"
		return
	fi
	check_parity "nested symbol" "${prog}"
}

test_dynsym_only() {
	local lib="${tmpdir}/libdyn.so"

	cat > "${tmpdir}/libdyn.c" << EOF
int dyn_only(int x)
{
	return x + 1;
}

int dyn_and_symtab(int x)
{
	return x * 2;
}

/* Same range as dyn_and_symtab; duplicate selection prefers the longer name. */
int dyn_and_symtab_alias(int x) __attribute__((alias("dyn_and_symtab")));
EOF
	if ! cc -O1 -shared -fPIC -o "${lib}" "${tmpdir}/libdyn.c" 2> /dev/null; then
		echo "Lazy-load parity .dynsym-only symbol [Skipped: cannot build]"
		return
	fi
	# --strip-symbol leaves .dynsym untouched.
	objcopy --strip-symbol=dyn_only --strip-symbol=dyn_and_symtab_alias \
		"${lib}"
	for sym in dyn_only dyn_and_symtab_alias; do
		if readelf -sW "${lib}" | \
			awk '/Symbol table .\.symtab/ { s = 1 } s' | \
			grep -qw "${sym}" || \
		   ! readelf --dyn-syms -W "${lib}" | grep -qw "${sym}"; then
			echo "Lazy-load parity .dynsym-only symbol [Skipped: cannot strip]"
			return
		fi
	done
	check_parity ".dynsym-only symbol" "${lib}"
}

test_gnu_debugdata() {
	local lib="${tmpdir}/libgdd.so"
	local dir="${tmpdir}/gdd"

	if ! perf check feature -q lzma || ! command -v xz > /dev/null; then
		echo "Lazy-load parity .gnu_debugdata [Skipped: no lzma support or xz]"
		return
	fi
	mkdir "${dir}"
	cat > "${dir}/libgdd.c" << EOF
int exported(int x)
{
	return x * 3 + 1;
}

/* Same range as exported; duplicate selection prefers the global symbol. */
static int local_alias(int x) __attribute__((alias("exported"), used));

__attribute__((noinline)) static int local_helper(int x)
{
	return x + 7;
}

int call_local(int x)
{
	return local_helper(x) * 2;
}
EOF
	if ! cc -O1 -shared -fPIC -o "${lib}" "${dir}/libgdd.c" 2> /dev/null; then
		echo "Lazy-load parity .gnu_debugdata [Skipped: cannot build]"
		return
	fi
	# Build MiniDebugInfo the way distributions do: the function symbols
	# that are not in .dynsym, xz-compressed into .gnu_debugdata.
	nm -D --format=posix --defined-only "${lib}" | awk '{ print $1 }' | \
		sort > "${dir}/dynsyms"
	nm --format=posix --defined-only "${lib}" | \
		awk '$2 == "T" || $2 == "t" { print $1 }' | sort > "${dir}/funcsyms"
	comm -13 "${dir}/dynsyms" "${dir}/funcsyms" > "${dir}/keep"
	if ! grep -qx local_alias "${dir}/keep" ||
	   ! objcopy --only-keep-debug "${lib}" "${dir}/debug" ||
	   ! objcopy -S --remove-section .comment \
		--keep-symbols="${dir}/keep" "${dir}/debug" "${dir}/mini" ||
	   ! xz "${dir}/mini" || ! strip --strip-all "${lib}" ||
	   ! objcopy --add-section .gnu_debugdata="${dir}/mini.xz" "${lib}" ||
	   readelf -SW "${lib}" | grep -qw '\.symtab'; then
		echo "Lazy-load parity .gnu_debugdata [Skipped: cannot build MiniDebugInfo]"
		return
	fi
	check_parity ".gnu_debugdata" "${lib}" eager
}

test_ifunc_alias() {
	local prog="${tmpdir}/ifunc"
	local order

	if [ "$(uname -m)" != x86_64 ]; then
		echo "Lazy-load parity IFUNC alias [Skipped: IRELATIVE naming is x86_64 only]"
		return
	fi
	# Duplicate selection prefers fewer leading underscores, so at the
	# resolver's address __lazy_ifunc loses to _lazy_ifunc_x, which loses
	# to lazy_ifunc_y. Whether the PLT slot is named after lazy_ifunc_y
	# depends on how that passes on the IFUNC mark.
	cat > "${prog}.c" << EOF
static int impl(void)
{
	return 42;
}

__attribute__((used, noinline)) static void *___lazy_ifunc_resolver(void)
{
	return (void *)impl;
}

asm(".type __lazy_ifunc, @gnu_indirect_function\n"
    ".set __lazy_ifunc, ___lazy_ifunc_resolver\n"
    ".size __lazy_ifunc, 1\n"
    ".type _lazy_ifunc_x, @function\n"
    ".set _lazy_ifunc_x, ___lazy_ifunc_resolver\n"
    ".size _lazy_ifunc_x, 1\n"
    ".type lazy_ifunc_y, @function\n"
    ".set lazy_ifunc_y, ___lazy_ifunc_resolver\n"
    ".size lazy_ifunc_y, 1\n");

int __lazy_ifunc(void);

int main(void)
{
	return __lazy_ifunc();
}
EOF
	if ! cc -O1 -fno-pie -no-pie -o "${prog}" "${prog}.c" 2> /dev/null ||
	   ! readelf -rW "${prog}" | grep -q R_X86_64_IRELATIVE; then
		echo "Lazy-load parity IFUNC alias [Skipped: cannot build]"
		return
	fi
	# The outcome depends on the order of the aliases in .symtab.
	order=$(readelf -sW "${prog}" | \
		awk '$8 ~ /^_*lazy_ifunc(_x|_y)?$/ { printf "%s ", $8 }')
	if [ "${order}" != "__lazy_ifunc _lazy_ifunc_x lazy_ifunc_y " ]; then
		echo "Lazy-load parity IFUNC alias [Skipped: unexpected symbol order]"
		return
	fi
	check_parity "IFUNC alias" "${prog}"
}

test_split_debug
test_nested_symbol
test_dynsym_only
test_gnu_debugdata
test_ifunc_alias

cleanup
exit ${err}
