#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Generate a syscall table header.
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
	echo >&2 "usage: $0 [--abis ABIS] [--common-tbl PATH] [--common-offset NR] INFILE OUTFILE" >&2
	echo >&2
	echo >&2 "  INFILE    input syscall table"
	echo >&2 "  OUTFILE   output header file"
	echo >&2
	echo >&2 "options:"
	echo >&2 "  --abis ABIS             ABI(s) to handle (By default, all lines are handled)"
	echo >&2 "  --common-tbl PATH       Use the common syscall number table"
	echo >&2 "  --common-offset OFFSET  Add an offset for the numbers of the common table"
	exit 1
}

# default unless specified by options
abis=
common_tbl=
common_tbl_path=
common_offset=

while [ $# -gt 0 ]
do
	case $1 in
	--abis)
		abis="($(echo "${2%,}" | tr ',' '|'))"
		shift 2;;
	--common-tbl)
		common_tbl=1
		common_tbl_path=$2
		shift 2;;
	--common-offset)
		common_offset=$2
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

nxt=0

# gen_tbl(infile, offset_nr)
gen_tbl() {
	input=$1
	offset_nr=$2

	while read nr abi name native compat noreturn; do

                if [ -n "$offset_nr" ]; then
                        nr=$((nr + offset_nr))
                fi

		if [ -n "$offset" ]; then
			nr="($offset + $nr)"
		fi

		if [ $nxt -gt $nr ]; then
			echo "error: $input: syscall table is not sorted or duplicates the same syscall number" >&2
			exit 1
		fi

		while [ $nxt -lt $nr ]; do
			echo "__SYSCALL($nxt, sys_ni_syscall)"
			nxt=$((nxt + 1))
		done

		if [ "$compat" = "-" ]; then
			unset compat
		fi

		if [ -n "$noreturn" ]; then
			if [ "$noreturn" != "noreturn" ]; then
				echo "error: $input: invalid string \"$noreturn\" in 'noreturn' column"
				exit 1
			fi
			if [ -n "$compat" ]; then
				echo "__SYSCALL_COMPAT_NORETURN($nr, $native, $compat)"
			else
				echo "__SYSCALL_NORETURN($nr, $native)"
			fi
		elif [ -n "$compat" ]; then
			echo "__SYSCALL_WITH_COMPAT($nr, $native, $compat)"
		elif [ -n "$native" ]; then
			echo "__SYSCALL($nr, $native)"
		else
			echo "__SYSCALL($nr, sys_ni_syscall)"
		fi
		nxt=$((nr + 1))

	done < <(grep -E "^[0-9]+[[:space:]]+$abis" "$input")

}

gen_tbl $infile  > "$outfile"

if [ -n "$common_tbl" ]; then
	gen_tbl $common_tbl_path $common_offset >> "$outfile"
fi
