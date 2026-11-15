#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2026, Red Hat Inc, Arnaldo Carvalho de Melo <acme@redhat.com>
#
# LLVM source-based coverage for perf: build with clang instrumentation,
# run the complete perf test suite and report line/function coverage for
# perf, optionally comparing it against a previously saved report.  -t
# narrows the tests and -f adds files to the per-file table; without -f
# the report lists the files with the most uncovered lines, which is
# where new tests are needed.  --table prints the last run as an ASCII
# coverage table, like pahole's coverage_table.py.  The build can be
# split from the test run: --prep builds the instrumented perf and
# records the build directory, and a later run as root uses that build,
# skips the build and goes straight to the tests.  The sampling based
# tests move the percentages by a percent or two between identical runs.

set -euo pipefail

# awk prints dots and printf/sort parse them: keep the numeric locale fixed
# so --table doesn't abort where the decimal separator is a comma.
export LC_ALL=C

PERF_DIR=$(cd "$(dirname "$0")/.." && pwd)

DEFAULT_BUILD_DIR=/tmp/build/perf-coverage
BUILD_DIR=${BUILD_DIR:-}
COV_DIR=${COV_DIR:-}
BUILD_DIR_SET=0
COV_DIR_SET=0
[ -n "$BUILD_DIR" ] && BUILD_DIR_SET=1
[ -n "$COV_DIR" ] && COV_DIR_SET=1
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 1)}
LLVM_PROFDATA=${LLVM_PROFDATA:-llvm-profdata}
LLVM_COV=${LLVM_COV:-llvm-cov}

DEBUG_BUILD=1
SKIP_BUILD=0
REPORT_ONLY=0
PREP=0
TABLE=0
HTML=0
USE_BASELINE=1
BASELINE=
UNCOVERED=${UNCOVERED:-20}
TOP=${TOP:-20}
MIN_LINES=${MIN_LINES:-100}
DWARF_VERSION=${DWARF_VERSION:-4}
TESTS=()

FOCUS=()

ROOT=0
[ "$(id -u)" = 0 ] && ROOT=1

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[0;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[0;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage()
{
	cat <<EOF
Usage: scripts/coverage.sh [options] [test name fragment...]

Builds perf with clang source-based coverage, runs the complete perf
test suite and reports coverage for perf.  Use -t to run a subset and
-f to focus the per-file table.

Options:
  -b, --build-dir DIR    perf build directory (default: $DEFAULT_BUILD_DIR)
  -c, --cov-dir DIR      profiles and reports (default: BUILD_DIR/cov)
  -j, --jobs N           parallel build jobs (default: $JOBS)
  -t, --tests "A;B"      test name fragments, ';' separated (default: all)
  -f, --focus FILE       add a file to the per-file table (repeatable)
      --top N            when no -f is given, show the N files with the
                         most uncovered lines (default: $TOP, 0 to disable)
      --baseline FILE    baseline report (default: COV_DIR/baseline.txt)
      --no-baseline      do not compare against or update a baseline
      --uncovered N      uncovered line numbers per focus file (default: $UNCOVERED)
      --skip-build       reuse the existing build, just run the tests
      --report-only      reuse build and profiles, just render the report
      --prep             build the instrumented perf and stop; run the
                         tests later as root with this script
  -T, --table            print an ASCII table of the last run and exit,
                         like pahole's coverage_table.py
      --optimized        build with perf's default -O3 instead of -O0 -g
      --dwarf-version N  DWARF version for the instrumented build (default: $DWARF_VERSION)
      --html             also write an HTML report
  -h, --help             show this help

Environment:
  BUILD_DIR, COV_DIR, JOBS, LLVM_COV, LLVM_PROFDATA, UNCOVERED, TOP,
  MIN_LINES, DWARF_VERSION

As root, without --prep, the script skips the build, checks the perf
prepared by 'make coverage-prep', runs the tests and renders the
report.  The prepared build is recorded in tools/perf/.coverage-build-dir.

Through make:
  make -C tools/perf coverage COVERAGE_ARGS="-t 'perf annotate'"
  make -C tools/perf coverage-prep O=/tmp/build/perf-tools-next
  make -C tools/perf coverage-table

Examples:
  scripts/coverage.sh                       # full suite, fresh build
  scripts/coverage.sh --prep                # build only, test later as root
  scripts/coverage.sh --report-only         # re-render the last run
  scripts/coverage.sh --table               # ASCII table of the last run
  scripts/coverage.sh -t "perf annotate"    # a single test fragment
  scripts/coverage.sh -f util/evsel.c       # add a file to the table
EOF
}

while [ $# -gt 0 ]; do
	arg=$1
	optval=
	case "$arg" in
	--*=*) optval=${arg#*=}; arg=${arg%%=*} ;;
	esac

	case "$arg" in
	-b|--build-dir)   BUILD_DIR=${optval:-${2?missing value}}; BUILD_DIR_SET=1
	                  [ -z "$optval" ] && shift ;;
	-c|--cov-dir)     COV_DIR=${optval:-${2?missing value}}; COV_DIR_SET=1
	                  [ -z "$optval" ] && shift ;;
	-j|--jobs)        JOBS=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	-f|--focus)       FOCUS+=("${optval:-${2?missing value}}"); [ -z "$optval" ] && shift ;;
	--baseline)       BASELINE=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	--uncovered)      UNCOVERED=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	--top)            TOP=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	--dwarf-version)  DWARF_VERSION=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	-t|--tests)       IFS=';' read -r -a _t <<< "${optval:-${2?missing value}}"
	                  TESTS+=("${_t[@]}"); [ -z "$optval" ] && shift ;;
	--no-baseline)    USE_BASELINE=0 ;;
	--skip-build)     SKIP_BUILD=1 ;;
	--report-only)    REPORT_ONLY=1; SKIP_BUILD=1 ;;
	--prep)           PREP=1 ;;
	-T|--table)       TABLE=1; REPORT_ONLY=1; SKIP_BUILD=1 ;;
	--optimized)      DEBUG_BUILD=0 ;;
	--html)           HTML=1 ;;
	-h|--help)        usage; exit 0 ;;
	-*)               die "unknown option: $arg (see --help)" ;;
	*)                TESTS+=("$arg") ;;
	esac
	shift
done

#
# The build directory is recorded by --prep (and by a full run) so a
# later root run, which does not build, finds it without arguments.
#
if [ "$BUILD_DIR_SET" = 0 ] && [ -z "$BUILD_DIR" ] && [ "$PREP" = 0 ] &&
   { [ "$ROOT" = 1 ] || [ "$SKIP_BUILD" = 1 ] ||
     [ "$REPORT_ONLY" = 1 ] || [ "$TABLE" = 1 ]; } &&
   [ -f "$PERF_DIR/.coverage-build-dir" ]; then
	read -r BUILD_DIR < "$PERF_DIR/.coverage-build-dir"
fi
[ -n "$BUILD_DIR" ] || BUILD_DIR=$DEFAULT_BUILD_DIR

if [ "$COV_DIR_SET" = 0 ] && [ -z "$COV_DIR" ] &&
   [ -f "$BUILD_DIR/cov/state" ]; then
	COV_DIR=$(awk -F= '$1 == "COV_DIR" { print $2; exit }' \
		"$BUILD_DIR/cov/state")
fi
[ -n "$COV_DIR" ] || COV_DIR=$BUILD_DIR/cov

# The tests run from the kernel tree root, so keep these absolute.
abspath()
{
	case "$1" in
	/*) printf '%s\n' "$1" ;;
	*)  printf '%s/%s\n' "$PWD" "$1" ;;
	esac
}
BUILD_DIR=$(abspath "$BUILD_DIR")
COV_DIR=$(abspath "$COV_DIR")

[ -n "$BASELINE" ] || BASELINE=$COV_DIR/baseline.txt
[[ $TOP =~ ^[0-9]+$ ]] || die "--top wants a number, not '$TOP'"
[[ $MIN_LINES =~ ^[0-9]+$ ]] || die "MIN_LINES wants a number, not '$MIN_LINES'"

# Root does not build: use the perf prepared by coverage-prep.
if [ "$ROOT" = 1 ] && [ "$PREP" = 0 ] && [ "$TABLE" = 0 ]; then
	SKIP_BUILD=1
	say "Running as root, using the coverage build in $BUILD_DIR"
fi

BUILD_LOG=$COV_DIR/build.log
TEST_LOG=$COV_DIR/tests.log
REPORT=$COV_DIR/report.txt
PROFDATA=$COV_DIR/perf.profdata
# Discovery point for a later root or --table run, so it stays under
# BUILD_DIR even when --cov-dir points elsewhere.
STATE_FILE=$BUILD_DIR/cov/state
POINTER=$PERF_DIR/.coverage-build-dir

#
# Sanity checks
#
[ -d "$PERF_DIR" ] || die "$PERF_DIR not found"

if [ "$TABLE" = 0 ]; then
	if [ "$PREP" = 0 ]; then
		command -v "$LLVM_COV" > /dev/null ||
			die "$LLVM_COV not found (set LLVM_COV)"
	fi
	if [ "$SKIP_BUILD" = 0 ]; then
		command -v clang > /dev/null ||
			die "clang not found, it is the only compiler with -fcoverage-mapping"
	fi
	if [ "$PREP" = 0 ] && [ "$REPORT_ONLY" = 0 ]; then
		command -v "$LLVM_PROFDATA" > /dev/null ||
			die "$LLVM_PROFDATA not found (set LLVM_PROFDATA)"
	fi
	mkdir -p "$COV_DIR"
fi

#
# Build: clang, DEBUG=1 gives -O0 -g like pahole's coverage build, which
# keeps the line mapping honest at the cost of slower test runs.  DWARF 4
# is the default; --dwarf-version 5 describes the workloads linked into
# perf with DW_OP_addrx, the encoding clang uses for globals in DWARF 5.
#
check_coverage_build()
{
	[ -x "$BUILD_DIR/perf" ] ||
		die "no $BUILD_DIR/perf, run 'make -C $PERF_DIR O=$BUILD_DIR coverage-prep' first"
	if command -v nm > /dev/null; then
		# No -q: grep reads all of nm's output, so pipefail does
		# not see a SIGPIPE from an early exit.
		nm "$BUILD_DIR/perf" 2>/dev/null |
			grep '__llvm_profile' > /dev/null ||
			die "$BUILD_DIR/perf was not built for coverage, run coverage-prep"
	else
		warn "nm not found, cannot verify that $BUILD_DIR/perf is instrumented"
	fi
}

write_state()
{
	mkdir -p "$(dirname "$STATE_FILE")" ||
		die "could not create $(dirname "$STATE_FILE")"
	{
		printf 'BUILD_DIR=%s\n' "$BUILD_DIR"
		printf 'COV_DIR=%s\n' "$COV_DIR"
		printf 'DEBUG_BUILD=%s\n' "$DEBUG_BUILD"
		printf 'DWARF_VERSION=%s\n' "$DWARF_VERSION"
	} > "$STATE_FILE" || die "could not write $STATE_FILE"

	if ! printf '%s\n' "$BUILD_DIR" > "$POINTER" 2>/dev/null; then
		warn "could not write $POINTER, root runs will need -b $BUILD_DIR"
	fi
}

build_perf()
{
	if [ "$SKIP_BUILD" = 1 ]; then
		check_coverage_build
		return
	fi

	say "Building perf in $BUILD_DIR"
	mkdir -p "$BUILD_DIR"
	local debug=()
	[ "$DEBUG_BUILD" = 1 ] && debug=(DEBUG=1)

	if ! make -C "$PERF_DIR" O="$BUILD_DIR" CC=clang \
		"${debug[@]+"${debug[@]}"}" \
		EXTRA_CFLAGS="-fprofile-instr-generate -fcoverage-mapping -gdwarf-$DWARF_VERSION" \
		-j"$JOBS" > "$BUILD_LOG" 2>&1; then
		tail -20 "$BUILD_LOG" >&2
		die "build failed, full log in $BUILD_LOG"
	fi
	tail -1 "$BUILD_LOG"
	write_state
}

#
# Run the selected tests with the instrumented perf in PATH: the shell
# tests, the workloads and 'perf test' itself all come from there.
#
run_tests()
{
	if [ ${#TESTS[@]} -eq 0 ]; then
		say "Running the complete perf test suite"
	else
		say "Running perf tests: ${TESTS[*]}"
	fi
	rm -rf "$COV_DIR/raw"
	mkdir -p "$COV_DIR/raw"

	set +e
	# The shell tests only get discovered from the kernel tree root.
	(cd "$PERF_DIR/../.." &&
		PATH="$BUILD_DIR:$PATH" \
		LLVM_PROFILE_FILE="$COV_DIR/raw/perf-%p-%m.profraw" \
			"$BUILD_DIR/perf" test -v \
				"${TESTS[@]+"${TESTS[@]}"}") 2>&1 | tee "$TEST_LOG"
	test_status=${PIPESTATUS[0]}
	set -e

	# 'perf test' exits 0 even when tests fail, only the summary tells.
	if [ "$test_status" = 0 ] && grep -qE '^Failed tests +: +[1-9]' "$TEST_LOG"; then
		test_status=1
	fi

	grep -E '^(Passed|Skipped|Failed) .*:' "$TEST_LOG" || true
	[ "$test_status" = 0 ] ||
		warn "some tests failed or were skipped, coverage is still collected"
	if [ "$test_status" != 0 ] && grep -q 'Access to performance monitoring' "$TEST_LOG"; then
		warn "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)" \
		     "denied events to tests recording with callchains;" \
		     "run as root or lower it for the full suite"
	fi
}

#
# Merge the per-process profiles and render the text report.
#
merge_profiles()
{
	local count size
	count=$(find "$COV_DIR/raw" -name '*.profraw' | wc -l)
	size=$(du -sh "$COV_DIR/raw" 2>/dev/null | cut -f1)
	[ "$count" -gt 0 ] ||
		die "no .profraw files, was perf rebuilt with -fprofile-instr-generate?"

	say "Merging $count profile files ($size)"
	"$LLVM_PROFDATA" merge -sparse "$COV_DIR"/raw/*.profraw -o "$PROFDATA" ||
		die "llvm-profdata merge failed"
}

llvm_cov_objects()
{
	echo "$BUILD_DIR/perf"
	local so
	for so in perf.so libperf.so libperf-jvmti.so; do
		[ -f "$BUILD_DIR/$so" ] && echo "--object=$BUILD_DIR/$so"
	done
	# The Python extension is named after the interpreter ABI and lives
	# in its own directory.
	for so in "$BUILD_DIR"/python/perf*.so; do
		[ -f "$so" ] && echo "--object=$so"
	done
}

render_report()
{
	local objects
	mapfile -t objects < <(llvm_cov_objects)

	say "Generating report"
	"$LLVM_COV" report "${objects[@]}" --instr-profile="$PROFDATA" > "$REPORT"
}

#
# Line coverage of a full report for one file: "lines missed cover fn%".
# Report paths are relative to the build directory, so match the requested
# path as a suffix: a build outside the source tree gives
# ".../tools/perf/..." and one inside gives "tools/perf/...".
#
# -f takes the path as it shows in the report, or a tools/perf/ relative one.
focus_path()
{
	case "$1" in
	*tools/perf/*) printf '%s\n' "${1#*tools/perf/}" ;;
	*) printf '%s\n' "$1" ;;
	esac
}

file_cov()
{
	awk -v f="$2" '
		function perf_relpath(p) {
			if (p ~ /(^|\/)tools\/perf\//)
				sub(/^.*tools\/perf\//, "", p)
			return p
		}
		{
			g = f
			sub(/^.*tools\/perf\//, "", g)
			rel = perf_relpath($1)
			if (rel == f || rel == g || $1 == f) {
				print $8, $9, $10, $7
				exit
			}
		}' "$1"
}

total_cov()
{
	awk '$1 == "TOTAL" { print $8, $9, $10 }' "$1"
}

total_delta()
{
	local prev=$1 cur=$2 old cur_cov
	[ -f "$prev" ] || return 0
	old=$(total_cov "$prev" | awk '{print $3}')
	cur_cov=$(total_cov "$cur" | awk '{print $3}')
	[ -n "$old" ] && [ -n "$cur_cov" ] || return 0
	awk -v cur="$cur_cov" -v old="$old" 'BEGIN {
		sub(/%$/, "", cur); sub(/%$/, "", old)
		printf "%+.2f%%", cur - old }'
}

report_delta()
{
	local prev=$1 cur=$2 label
	local cur_cov old_cov delta
	label=$(focus_path "$3")
	cur_cov=$(file_cov "$cur" "$label" | awk '{print $3}')
	[ -n "$cur_cov" ] || return 0

	if [ -f "$prev" ]; then
		old_cov=$(file_cov "$prev" "$label" | awk '{print $3}')
		if [ -n "$old_cov" ]; then
			delta=$(awk -v cur="$cur_cov" -v old="$old_cov" 'BEGIN {
				sub(/%$/, "", cur); sub(/%$/, "", old)
				printf "%+.2f%%", cur - old }')
		else
			delta="(new)"
		fi
	else
		old_cov="-"
		delta="-"
	fi
	printf "  %-36s %8s  %8s  %9s\n" "$label" "${old_cov:--}" "$cur_cov" "$delta"
}

#
# With no -f, show where the uncovered lines are: the files with the most
# missed lines are where new tests should try to get more coverage.
#
print_worst_files()
{
	[ "$TOP" -gt 0 ] || return 0
	local rows missed lines cover fn path
	rows=$(awk -v min="$MIN_LINES" '
		$1 ~ /(^|\/)tools\/perf\// && $8 >= min && $9 > 0 {
			path = $1
			sub(/^.*tools\/perf\//, "", path)
			printf "%s %s %s %s %s\n", $9, $8, $10, $7, path
		}' "$REPORT" | sort -k1,1nr -k2,2nr | sed -n "1,${TOP}p")
	[ -n "$rows" ] || return 0

	printf "  largest uncovered files (>= %s lines)\n" "$MIN_LINES"
	printf "  %-44s %8s  %8s  %8s  %8s\n" \
		"file" "lines" "cover" "missed" "fn-cover"
	while read -r missed lines cover fn path; do
		printf "  %-44s %8s  %8s  %8s  %8s\n" \
			"$path" "$lines" "$cover" "$missed" "$fn"
	done <<< "$rows"
}

print_summary()
{
	local lines missed cover delta file cov
	read -r lines missed cover < <(total_cov "$REPORT")
	printf "\n\033[1mTotal:\033[0m %s of %s lines covered (%s)" \
		"$((lines - missed))" "$lines" "$cover"
	if [ "$USE_BASELINE" = 1 ] && [ -f "$BASELINE" ] &&
	   ! cmp -s "$BASELINE" "$REPORT"; then
		delta=$(total_delta "$BASELINE" "$REPORT")
		[ -n "$delta" ] && printf "  (previous %s, %s)" \
			"$(total_cov "$BASELINE" | awk '{print $3}')" "$delta"
	fi
	printf "\n\n"

	if [ ${#FOCUS[@]} -eq 0 ]; then
		print_worst_files
		return
	fi

	printf "  \033[1m%-36s\033[0m %8s  %6s  %6s  %8s\n" \
		"file" "lines" "cover" "missed" "fn-cover"
	for file in "${FOCUS[@]}"; do
		file=$(focus_path "$file")
		cov=$(file_cov "$REPORT" "$file")
		if [ -z "$cov" ]; then
			printf "  %-36s %8s\n" "$file" "not covered"
			continue
		fi
		printf "  %-36s %8s  %6s  %6s  %8s\n" "$file" \
			"$(echo "$cov" | awk '{print $1}')" \
			"$(echo "$cov" | awk '{print $3}')" \
			"$(echo "$cov" | awk '{print $2}')" \
			"$(echo "$cov" | awk '{print $4}')"
	done

	if [ "$USE_BASELINE" = 1 ] && [ -f "$BASELINE" ] &&
	   ! cmp -s "$BASELINE" "$REPORT"; then
		printf "\n  per file delta since %s\n" "$BASELINE"
		printf "  %-36s %8s  %8s  %9s\n" "file" "previous" "current" "delta"
		for file in "${FOCUS[@]}"; do
			report_delta "$BASELINE" "$REPORT" "$file"
		done
	fi
}

#
# --table: render the report of the last run as an ASCII table, like
# pahole's coverage_table.py.  Without -f the rows are the TOP files with
# the most uncovered lines, sorted by line coverage; --top 0 lists every
# file with at least MIN_LINES lines and uncovered lines.  The total is
# the whole report, which also counts the libraries perf links with.
#
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
	awk -v f="$2" '
		function perf_relpath(p) {
			if (p ~ /(^|\/)tools\/perf\//)
				sub(/^.*tools\/perf\//, "", p)
			return p
		}
		{
			g = f
			sub(/^.*tools\/perf\//, "", g)
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
	if [ ${#FOCUS[@]} -gt 0 ]; then
		local file
		for file in "${FOCUS[@]}"; do
			table_row "$REPORT" "$file"
		done | sort -t$'\t' -k5,5gr
		return
	fi

	awk -v min="$MIN_LINES" '
		$1 ~ /(^|\/)tools\/perf\// && $8 >= min && $9 > 0 {
			path = $1
			sub(/^.*tools\/perf\//, "", path)
			printf "%s\t%s\t%d\t%d\t%.2f\t%s\t%d\t%s\n",
			       path, $8, $8 - $9, $9, $10, $5, $5 - $6, $7
		}' "$REPORT" |
		sort -t$'\t' -k4,4nr |
		if [ "$TOP" -gt 0 ]; then sed -n "1,${TOP}p"; else cat; fi |
		sort -t$'\t' -k5,5gr
}

print_table()
{
	local rows total path lines hit missed pct funcs funcs_hit func_cov
	local row_fmt w d_file d29 d13 d10 d8

	rows=$(table_rows)
	[ -n "$rows" ] ||
		die "no coverage data in $REPORT for the requested files"
	total=$(awk '$1 == "TOTAL" {
		printf "%s\t%d\t%d\t%.2f\t%s\t%d\t%s\n",
		       $8, $8 - $9, $9, $10, $5, $5 - $6, $7
	}' "$REPORT")

	w=4
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
		printf "$row_fmt" "TOTAL" "$lines" "$hit" "$missed" \
			"$(bar "$pct" 20)" "$pct" "$funcs_hit" "$funcs" "$func_cov"
	done <<< "$total"
	printf '└%s┴%s┴%s┴%s┴%s┴%s┴%s┘\n' \
		"$d_file" "$d8" "$d8" "$d8" "$d29" "$d13" "$d10"
}

print_uncovered()
{
	[ "$UNCOVERED" -gt 0 ] || return 0
	[ ${#FOCUS[@]} -gt 0 ] || return 0
	local objects file out lines header=0
	mapfile -t objects < <(llvm_cov_objects)

	for file in "${FOCUS[@]}"; do
		file=$(focus_path "$file")
		# No report row means the file was not built: print_summary()
		# already said so, llvm-cov show would dump the whole profile.
		[ -n "$(file_cov "$REPORT" "$file")" ] || continue
		out=$("$LLVM_COV" show "${objects[@]}" --instr-profile="$PROFDATA" \
			"$PERF_DIR/$file" 2>/dev/null |
			awk -F'|' '$2 ~ /^[[:space:]]*0[[:space:]]*$/ {
				gsub(/[[:space:]]/, "", $1); printf "%s ", $1 }')
		lines=$(echo "$out" | wc -w)
		[ "$lines" -gt 0 ] || continue
		if [ "$header" = 0 ]; then
			printf "\n  uncovered lines (first %s per file)\n" "$UNCOVERED"
			header=1
		fi
		printf "  %-24s %4s lines: %s\n" "$file" "$lines" \
			"$(echo "$out" | cut -d' ' -f1-"$UNCOVERED")"
	done
}

save_html()
{
	[ "$HTML" = 1 ] || return 0
	local objects
	mapfile -t objects < <(llvm_cov_objects)
	say "Writing HTML report to $COV_DIR/html"
	"$LLVM_COV" show "${objects[@]}" --instr-profile="$PROFDATA" \
		--format=html --output-dir="$COV_DIR/html" \
		--show-line-counts-or-regions --show-branches=count
}

check_stale()
{
	[ "$REPORT_ONLY" = 1 ] || return 0
	if [ ! -f "$PROFDATA" ]; then
		[ "$TABLE" = 1 ] && return 0
		die "no $PROFDATA, run without --report-only first"
	fi
	local newer
	newer=$(find "$PERF_DIR" -name '*.c' -newer "$PROFDATA" -print -quit 2>/dev/null)
	if [ -n "$newer" ]; then
		warn "profiles are older than source changes (e.g. $newer), results may be stale"
	fi
}

update_baseline()
{
	[ "$USE_BASELINE" = 1 ] || return 0
	[ "$REPORT_ONLY" = 1 ] && return 0
	[ -f "$BASELINE" ] && mv "$BASELINE" "$BASELINE.prev"
	cp "$REPORT" "$BASELINE"
	printf "\nBaseline saved: %s\n" "$BASELINE"
}

test_status=0

if [ "$TABLE" = 1 ]; then
	[ -f "$REPORT" ] || die "no $REPORT, run coverage first"
	check_stale
	print_table
	exit 0
fi

if [ "$PREP" = 1 ]; then
	build_perf
	write_state
	say "Coverage build ready: $BUILD_DIR/perf"
	printf "As root: %s/scripts/coverage.sh\n" "$PERF_DIR"
	exit 0
fi

if [ "$REPORT_ONLY" = 0 ]; then
	build_perf
	run_tests
	merge_profiles
else
	check_coverage_build
	check_stale
fi

[ -f "$PROFDATA" ] || die "no $PROFDATA, run without --report-only first"

render_report
print_summary
print_uncovered
save_html
update_baseline

printf "\nReport:  %s\n" "$REPORT"
[ "$HTML" = 1 ] && printf "HTML:    %s\n" "$COV_DIR/html/index.html"

exit "$test_status"
