#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Generate a syscall number header.
#
# Each line of the syscall table should have the following format:
#
# NR ABI NAME [NATIVE] [COMPAT]
#
# NR       syscall number
# ABI      ABI name
# NAME     syscall name
# NATIVE   native entry point (optional)
# COMPAT   compat entry point (optional)

set -e

usage() {
	echo >&2 "usage: $0 [--abis ABIS] [--emit-nr] [--offset OFFSET] [--prefix PREFIX] [--common-tbl] INFILE OUTFILE" >&2
	echo >&2
	echo >&2 "  INFILE    input syscall table"
	echo >&2 "  OUTFILE   output header file"
	echo >&2
	echo >&2 "options:"
	echo >&2 "  --abis ABIS        ABI(s) to handle (By default, all lines are handled)"
	echo >&2 "  --emit-nr          Emit the macro of the number of syscalls (__NR_syscalls)"
	echo >&2 "  --offset OFFSET    The offset of syscall numbers"
	echo >&2 "  --prefix PREFIX    The prefix to the macro like __NR_<PREFIX><NAME>"
	echo >&2 "  --common-tbl PATH  Use the common number table"
	exit 1
}

# default unless specified by options
abis=
emit_nr=
offset=
prefix=
common_tbl=
common_tbl_path=

while [ $# -gt 0 ]
do
	case $1 in
	--abis)
		abis="($(echo "${2%,}" | tr ',' '|'))"
		shift 2;;
	--emit-nr)
		emit_nr=1
		shift 1;;
	--offset)
		offset=$2
		shift 2;;
	--prefix)
		prefix=$2
		shift 2;;
	--common-tbl)
		common_tbl=1
		common_tbl_path=$2
		shift 2;;
	-*)
		echo "$1: unknown option" >&2
		usage;;
	*)
		break;;
	esac
done

if [ $# -ne 2 ]; then
	usage
fi

infile="$1"
outfile="$2"

guard=_UAPI_ASM_$(basename "$outfile" |
	sed -e 'y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/' \
	-e 's/[^A-Z0-9_]/_/g' -e 's/__/_/g')

emit_start_guard() {
	echo "#ifndef $guard"
	echo "#define $guard"
	echo
}

# emit_nr(prefix, nr)
emit_nr() {
	local nr=$1
	local prefix=$2
	echo
	echo "#ifdef __KERNEL__"
	echo "#define __NR_${prefix}syscalls $nr"
	echo "#endif"
}

emit_end_guard() {
	echo
	echo "#endif /* $guard */"
}

max=0
# gen_hdr(infile)
gen_hdr() {
	input=$1

	while read nr abi name native compat; do

		max=$nr

		if [ -n "$offset" ]; then
			nr="($offset + $nr)"
		fi

		echo "#define __NR_$prefix$name $nr"

	done < <(grep -E "^[0-9A-Fa-fXx]+[[:space:]]+$abis" "$input")
}

emit_start_guard > $outfile

gen_hdr $infile >> $outfile

if [ -n "$common_tbl" ]; then
	gen_hdr $common_tbl_path  >> "$outfile"
fi

if [ -n "$emit_nr" ]; then
	emit_nr $(($max + 1)) $prefix >> $outfile
fi

emit_end_guard >> $outfile
