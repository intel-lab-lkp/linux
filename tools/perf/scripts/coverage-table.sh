#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2026, Red Hat Inc, Arnaldo Carvalho de Melo <acme@redhat.com>
#
# Render the report of the last coverage run as a text table, like pahole's
# coverage_table.py.  Without -f the default is the tool's files, not its
# tests and benches, with the most uncovered lines, biggest gap first;
# --all lists every tools/perf file, most covered first.  The total is the
# tools/perf subtotal, the run summary has the whole report.

set -euo pipefail

PERF_DIR=$(cd "$(dirname "$0")/.." && pwd -P)

BUILD_DIR=${BUILD_DIR:-}
COV_DIR=${COV_DIR:-}
TOP=${TOP:-20}
MIN_LINES=${MIN_LINES:-100}
TABLE_ALL=0
FOCUS=()

die()  { printf '\033[0;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage()
{
	cat <<EOF
Usage: scripts/coverage-table.sh [options]

Render the report of the last coverage run as a text table.  Run
scripts/coverage.sh first to produce the report.

Options:
  -b, --build-dir DIR    build directory of the run (default: $PERF_DIR)
  -c, --cov-dir DIR      directory with report.txt (default: BUILD_DIR/cov)
  -f, --focus FILE       add a file to the table (repeatable)
      --top N            default view: the N files with the most uncovered
                         lines (default: $TOP, 0 to disable)
      --all              list every tools/perf file, most covered first
      --min-lines N      default view: only files with N+ executable
                         lines (default: $MIN_LINES)
  -h, --help             show this help

Environment: BUILD_DIR, COV_DIR, TOP, MIN_LINES
EOF
}

while [ $# -gt 0 ]; do
	arg=$1
	optval=
	case "$arg" in
	--*=*) optval=${arg#*=}; arg=${arg%%=*}
	       [ -n "$optval" ] || die "$arg needs a value" ;;
	esac

	case "$arg" in
	--all|-h|--help)
		[ -z "$optval" ] || die "$arg takes no value" ;;
	esac

	case "$arg" in
	-b|--build-dir) BUILD_DIR=${optval:-${2:?$arg needs a value}}
	                [ -z "$optval" ] && shift ;;
	-c|--cov-dir)   COV_DIR=${optval:-${2:?$arg needs a value}}
	                [ -z "$optval" ] && shift ;;
	-f|--focus)     FOCUS+=("${optval:-${2:?$arg needs a value}}")
	                [ -z "$optval" ] && shift ;;
	--top)          TOP=${optval:-${2:?$arg needs a value}}
	                [ -z "$optval" ] && shift ;;
	--min-lines)    MIN_LINES=${optval:-${2:?$arg needs a value}}
	                [ -z "$optval" ] && shift ;;
	--all)          TABLE_ALL=1 ;;
	-h|--help)      usage; exit 0 ;;
	-*)             die "unknown option: $arg (see --help)" ;;
	*)              die "unexpected argument: $arg (see --help)" ;;
	esac
	shift
done

[ -n "$BUILD_DIR" ] || BUILD_DIR=$PERF_DIR
BUILD_DIR=$(realpath -m "$BUILD_DIR")
[ -n "$COV_DIR" ] || COV_DIR=$BUILD_DIR/cov
COV_DIR=$(realpath -m "$COV_DIR")
REPORT=$COV_DIR/report.txt

[ -f "$REPORT" ] || die "no $REPORT, run coverage first"
[[ $TOP =~ ^[0-9]+$ ]] || die "--top wants a number, not '$TOP'"
[[ $MIN_LINES =~ ^[0-9]+$ ]] ||
	die "--min-lines wants a number, not '$MIN_LINES'"

# An in-tree build has the sources below BUILD_DIR, so only a separate
# build directory can tell generated files apart from the tree's sources.
BUILD_AWK_DIR=$BUILD_DIR
[ "$BUILD_DIR" = "$PERF_DIR" ] && BUILD_AWK_DIR=

# llvm-cov strips the prefix every reported file has in common, so a
# tools/perf source can read as perf/util/evsel.c.
AWK_PERF='
function perf_relpath(p) {
	sub(/^.*tools\/perf\//, "", p)
	sub(/^perf\//, "", p)
	return p
}
function under_build(p,    b, n) {
	if (build == "")
		return 0
	b = build
	while (1) {
		n = length(b)
		if (substr(p, 1, n + 1) == b "/")
			return 1
		if (index(b, "/") == 0)
			return 0
		sub(/^[^\/]*\//, "", b)
		if (b == "")
			return 0
	}
}
function generated(p,    r) {
	r = perf_relpath(p)
	if (under_build(p))
		return 1
	return r ~ /^(lib(api|bpf|perf|subcmd|symbol)|trace\/beauty\/generated)\// ||
	       r ~ /^arch\/[^\/]+\/include\/generated\// ||
	       r ~ /^pmu-events\/(pmu-events(-string)?|test-empty-pmu-events)\.c$/ ||
	       r ~ /^util\/(intel-pt-decoder\/inat-tables\.c|.*-(bison|flex)\.[ch])$/ ||
	       r ~ /^util\/bpf_skel\/(\.tmp\/|vmlinux\.h$)/ ||
	       r ~ /\.skel\.h$/ ||
	       r == "common-cmds.h"
}
'

bar()
{
	local pct=$1 width=$2 filled i out=
	filled=$(awk -v p="$pct" -v w="$width" 'BEGIN { printf "%d", p * w / 100 }')
	for ((i = 0; i < filled; i++)); do out+=█; done
	for ((i = filled; i < width; i++)); do out+=░; done
	printf '%s' "$out"
}

center()
{
	local left=$(( ($2 - ${#1}) / 2 ))
	printf '%*s%s%*s' "$left" '' "$1" "$(( $2 - ${#1} - left ))" ''
}

dashes()
{
	local s
	printf -v s '%*s' "$1" ''
	printf '%s' "${s// /─}"
}

table_row()
{
	awk -v f="$2" "$AWK_PERF"'
		{
			g = perf_relpath(f)
			path = perf_relpath($1)
			if (path == f || path == g || $1 == f) {
				printf "%s\t%s\t%d\t%d\t%.2f\t%s\t%d\t%s\n",
				       path, $8, $8 - $9, $9, $10, $5, $5 - $6, $7
				exit
			}
		}' "$1"
}

table_rows()
{
	local file

	if [ ${#FOCUS[@]} -gt 0 ]; then
		for file in "${FOCUS[@]}"; do
			table_row "$REPORT" "$file"
		done | sort -t$'\t' -k5,5gr
		return
	fi

	if [ "$TABLE_ALL" = 1 ]; then
		awk -v build="$BUILD_AWK_DIR" "$AWK_PERF"'
			($1 ~ /(^|\/)tools\/perf\// || $1 ~ /^perf\//) &&
			!generated($1) && $8 > 0 {
				printf "%s\t%s\t%d\t%d\t%.2f\t%s\t%d\t%s\n",
				       perf_relpath($1), $8, $8 - $9, $9, $10,
				       $5, $5 - $6, $7
			}' "$REPORT" |
			sort -t$'\t' -k5,5gr
		return
	fi

	awk -v min="$MIN_LINES" -v build="$BUILD_AWK_DIR" "$AWK_PERF"'
		($1 ~ /(^|\/)tools\/perf\// || $1 ~ /^perf\//) &&
		!generated($1) && $8 >= min && $9 > 0 &&
		perf_relpath($1) !~ /^(tests|bench)\// {
			printf "%s\t%s\t%d\t%d\t%.2f\t%s\t%d\t%s\n",
			       perf_relpath($1), $8, $8 - $9, $9, $10,
			       $5, $5 - $6, $7
		}' "$REPORT" |
		sort -t$'\t' -k4,4nr |
		if [ "$TOP" -gt 0 ]; then
			sed -n "1,${TOP}p"
		else
			cat
		fi
}

# The table is about tools/perf, so its total is the tools/perf subtotal;
# the run summary prints the whole report, generated files included.
table_total()
{
	awk -v build="$BUILD_AWK_DIR" "$AWK_PERF"'
		$1 != "TOTAL" && !generated($1) &&
		($1 ~ /(^|\/)tools\/perf\// || $1 ~ /^perf\//) {
			lines += $8
			missed += $9
			funcs += $5
			funcs_missed += $6
		}
		END {
			printf "%d\t%d\t%d\t%.2f\t%d\t%d\t%.2f%%\n",
			       lines, lines - missed, missed,
			       lines ? 100 * (lines - missed) / lines : 0,
			       funcs, funcs - funcs_missed,
			       funcs ? 100 * (funcs - funcs_missed) / funcs : 0
		}' "$REPORT"
}

# At most three lines: what the table shows and how to see every file.
table_caption()
{
	if [ ${#FOCUS[@]} -gt 0 ]; then
		echo "files requested with -f."
	elif [ "$TABLE_ALL" = 1 ]; then
		echo "every tools/perf file, tests and benches included, most covered first."
		return
	elif [ "$TOP" -gt 0 ] && [ "$MIN_LINES" -gt 0 ]; then
		printf "tools/perf (no tests/ or bench/), %d+ executable lines:\n" "$MIN_LINES"
		echo "the $TOP biggest uncovered gaps, start here when adding coverage tests."
	elif [ "$TOP" -gt 0 ]; then
		printf "tools/perf (no tests/ or bench/): the %d files with the\n" "$TOP"
		echo "biggest uncovered gaps, start here when adding coverage tests."
	elif [ "$MIN_LINES" -gt 0 ]; then
		printf "tools/perf (no tests/ or bench/), %d+ executable lines:\n" "$MIN_LINES"
		echo "start here when adding coverage tests, biggest gaps first."
	else
		echo "tools/perf (no tests/ or bench/), biggest gaps first."
	fi
	local cmd='make -C tools/perf coverage-table'
	[ "$BUILD_DIR" = "$PERF_DIR" ] || cmd+=" O=$BUILD_DIR"
	printf '%s %s COVERAGE_ARGS="--all"\n' '--all for every file:' "$cmd"
}

print_table()
{
	local rows total path lines hit missed pct funcs funcs_hit func_cov
	local row_fmt w d_file d29 d13 d10 d8

	rows=$(table_rows)
	[ -n "$rows" ] ||
		die "no coverage data in $REPORT for the requested files"
	total=$(table_total)
	[ -n "$total" ] || die "no coverage data in $REPORT"

	table_caption
	printf '\n'

	w=12	# "TOTAL (perf)"
	while IFS=$'\t' read -r path _; do
		if [ ${#path} -gt "$w" ]; then
			w=${#path}
		fi
	done <<< "$rows"

	d8=$(dashes 8)
	d10=$(dashes 10)
	d13=$(dashes 13)
	d29=$(dashes 29)
	d_file=$(dashes $((w + 2)))

	printf '┌%s┬%s┬%s┬%s┬%s┬%s┬%s┐\n' \
		"$d_file" "$d8" "$d8" "$d8" "$d29" "$d13" "$d10"
	printf '│ %-*s │ %6s │ %6s │ %6s │ %s │ %11s │ %8s │\n' \
		"$w" "File" "Lines" "Hit" "Miss" \
		"$(center 'Line Coverage' 27)" "Funcs" "FuncCov"
	printf '├%s┼%s┼%s┼%s┼%s┼%s┼%s┤\n' \
		"$d_file" "$d8" "$d8" "$d8" "$d29" "$d13" "$d10"

	row_fmt=$(printf '│ %%-%ds │ %%6s │ %%6s │ %%6s │ %%s %%5.1f%%%%' "$w")
	row_fmt+=' │ %5s/%-5s │ %8s │\n'

	while IFS=$'\t' read -r path lines hit missed pct funcs funcs_hit func_cov; do
		printf "$row_fmt" "$path" "$lines" "$hit" "$missed" \
			"$(bar "$pct" 20)" "$pct" "$funcs_hit" "$funcs" "$func_cov"
	done <<< "$rows"

	printf '├%s┼%s┼%s┼%s┼%s┼%s┼%s┤\n' \
		"$d_file" "$d8" "$d8" "$d8" "$d29" "$d13" "$d10"
	while IFS=$'\t' read -r lines hit missed pct funcs funcs_hit func_cov; do
		printf "$row_fmt" "TOTAL (perf)" "$lines" "$hit" "$missed" \
			"$(bar "$pct" 20)" "$pct" "$funcs_hit" "$funcs" "$func_cov"
	done <<< "$total"
	printf '└%s┴%s┴%s┴%s┴%s┴%s┴%s┘\n' \
		"$d_file" "$d8" "$d8" "$d8" "$d29" "$d13" "$d10"
}

# The table code parses llvm-cov's decimal points; keep the numeric locale
# fixed.
export LC_ALL=C
print_table
