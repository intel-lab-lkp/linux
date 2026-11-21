#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2026, Red Hat Inc, Arnaldo Carvalho de Melo <acme@redhat.com>
#
# Build perf with clang source based coverage, run the perf test suite and
# report line coverage.  coverage-table.sh renders the last report as a
# table of the files with the most uncovered lines.
# The build goes into tools/perf and the profiles and report into
# tools/perf/cov; -b and -c override them, following perf's O= layout.
# --prep stops after the instrumented build, for running the tests later as
# root with the same build directory.

set -euo pipefail

PERF_DIR=$(cd "$(dirname "$0")/.." && pwd -P)
TOOLS_DIR=$(cd "$PERF_DIR/.." && pwd -P)
SRC_DIR=$(cd "$TOOLS_DIR/.." && pwd -P)

# perf links objects from these directories, so a change in any of them
# makes the build stale.
PERF_SOURCES=("$PERF_DIR" "$TOOLS_DIR/lib/perf" "$TOOLS_DIR/lib/api"
	      "$TOOLS_DIR/lib/subcmd" "$TOOLS_DIR/lib/bpf"
	      "$TOOLS_DIR/lib/symbol" "$TOOLS_DIR/include"
	      "$TOOLS_DIR/arch")

BUILD_DIR=${BUILD_DIR:-}
COV_DIR=${COV_DIR:-}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 1)}
LLVM_PROFDATA=${LLVM_PROFDATA:-llvm-profdata}
LLVM_COV=${LLVM_COV:-llvm-cov}
DWARF_VERSION=${DWARF_VERSION:-4}

PREP=0
SKIP_BUILD=0
USE_BASELINE=1
TESTS=()

ROOT=0
[ "$(id -u)" = 0 ] && ROOT=1

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[0;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[0;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage()
{
	cat <<EOF
Usage: scripts/coverage.sh [options] [test name fragment...]

Builds perf with clang source based coverage, runs the complete perf test
suite and reports line coverage.  Use -t to run a subset and
scripts/coverage-table.sh to render the last report as a table.

Options:
  -b, --build-dir DIR    existing perf build directory (default: $PERF_DIR)
  -c, --cov-dir DIR      profiles and report (default: BUILD_DIR/cov)
  -j, --jobs N           parallel build jobs (default: $JOBS)
  -t, --tests "A;B"      test name fragments, ';' separated (default: all)
      --no-baseline      do not compare against or update the baseline
      --prep             build the instrumented perf and stop
  -h, --help             show this help

Environment: BUILD_DIR, COV_DIR, JOBS, LLVM_COV, LLVM_PROFDATA,
DWARF_VERSION

Through make:
  make -C tools/perf coverage
  make -C tools/perf coverage-prep O=/tmp/build/perf-coverage
  sudo make -C tools/perf coverage O=/tmp/build/perf-coverage
  make -C tools/perf coverage-table O=/tmp/build/perf-coverage
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--build-dir=*) BUILD_DIR=${1#*=} ;;
	--cov-dir=*)   COV_DIR=${1#*=} ;;
	--jobs=*)      JOBS=${1#*=} ;;
	--tests=*)     IFS=';' read -r -a _t <<< "${1#*=}"
		       TESTS+=("${_t[@]}") ;;
	-b|--build-dir) BUILD_DIR=${2:?$1 needs a value}; shift ;;
	-c|--cov-dir)   COV_DIR=${2:?$1 needs a value}; shift ;;
	-j|--jobs)      JOBS=${2:?$1 needs a value}; shift ;;
	-t|--tests)     IFS=';' read -r -a _t <<< "${2:?$1 needs a value}"
			TESTS+=("${_t[@]}"); shift ;;
	--no-baseline)  USE_BASELINE=0 ;;
	--prep)         PREP=1 ;;
	-h|--help)      usage; exit 0 ;;
	-*)             die "unknown option: $1 (see --help)" ;;
	*)              TESTS+=("$1") ;;
	esac
	shift
done

[ -n "$BUILD_DIR" ] || BUILD_DIR=$PERF_DIR
BUILD_DIR=$(realpath -m "$BUILD_DIR")
[ -d "$BUILD_DIR" ] ||
	die "$BUILD_DIR does not exist; create it first, as with 'make O=DIR'"

[ -n "$COV_DIR" ] || COV_DIR=$BUILD_DIR/cov
COV_DIR=$(realpath -m "$COV_DIR")

# An in-tree build has the sources below BUILD_DIR, so only a separate
# build directory can tell generated files apart from the tree's sources.
BUILD_AWK_DIR=$BUILD_DIR
[ "$BUILD_DIR" = "$PERF_DIR" ] && BUILD_AWK_DIR=

[[ $JOBS =~ ^[0-9]+$ ]] || die "--jobs wants a number, not '$JOBS'"
[[ $DWARF_VERSION =~ ^[0-9]+$ ]] ||
	die "DWARF_VERSION wants a number, not '$DWARF_VERSION'"

#
# The tests execute $BUILD_DIR/perf, root included: refuse a directory
# owned by somebody else or writable by another user.
#
TRUST_UID=$(id -u)
if [ "$ROOT" = 1 ]; then
	if [ -n "${SUDO_UID:-}" ]; then
		TRUST_UID=$SUDO_UID
	else
		TRUST_UID=$(stat -c %u "$PERF_DIR" 2>/dev/null) ||
			die "could not stat $PERF_DIR"
	fi
fi

check_owner()
{
	local path=$1 uid gid mode

	[ -e "$path" ] || return 0
	read -r uid gid mode < <(stat -c '%u %g %a' "$path" 2>/dev/null) ||
		die "could not stat $path"
	[ "$uid" = 0 ] || [ "$uid" = "$TRUST_UID" ] ||
		die "$path is owned by uid $uid, not by root or uid $TRUST_UID"
	[ $(( 8#$mode & 8#022 )) = 0 ] ||
		die "$path is writable by another user"
}

check_owner "$BUILD_DIR"
check_owner "$BUILD_DIR/perf"

mkdir -p "$COV_DIR" || die "could not create $COV_DIR"
check_owner "$COV_DIR"

# A sudo run leaves the profiles and report owned by root; give them back
# to the developer so the next run can replace them.
if [ "$ROOT" = 1 ] && [ -n "${SUDO_UID:-}" ]; then
	trap 'chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$COV_DIR" 2>/dev/null || true' EXIT
fi

BUILD_LOG=$COV_DIR/build.log
TEST_LOG=$COV_DIR/tests.log
REPORT=$COV_DIR/report.txt
PROFDATA=$COV_DIR/perf.profdata
BASELINE=$COV_DIR/baseline.txt

#
# Build: clang, DEBUG=1 gives -O0 -g, which keeps the line mapping honest
# at the cost of slower test runs.
#
build_perf()
{
	local make_args=()

	if [ "$SKIP_BUILD" = 1 ]; then
		[ -x "$BUILD_DIR/perf" ] ||
			die "no $BUILD_DIR/perf, run coverage-prep first"
		if command -v nm > /dev/null; then
			# No -q: grep reads all of nm's output, so pipefail
			# does not see a SIGPIPE from an early exit.
			nm "$BUILD_DIR/perf" 2>/dev/null |
				grep '__llvm_profile' > /dev/null ||
				die "$BUILD_DIR/perf was not built for coverage," \
				    "run coverage-prep first"
		else
			warn "nm not found, cannot verify that $BUILD_DIR/perf is instrumented"
		fi
		return
	fi

	say "Building perf in $BUILD_DIR"
	[ "$BUILD_DIR" = "$PERF_DIR" ] || make_args+=(O="$BUILD_DIR")
	if ! make -C "$PERF_DIR" "${make_args[@]+"${make_args[@]}"}" \
		CC=clang DEBUG=1 \
		EXTRA_CFLAGS="-fprofile-instr-generate -fcoverage-mapping -gdwarf-$DWARF_VERSION" \
		-j"$JOBS" > "$BUILD_LOG" 2>&1; then
		tail -20 "$BUILD_LOG" >&2
		die "build failed, full log in $BUILD_LOG"
	fi
	tail -1 "$BUILD_LOG"
}

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
	# The shell tests are found from the kernel tree root.
	(cd "$SRC_DIR" &&
		PATH="$BUILD_DIR:$PATH" \
		LLVM_PROFILE_FILE="$COV_DIR/raw/perf-%p-%m.profraw" \
			"$BUILD_DIR/perf" test -v \
				"${TESTS[@]+"${TESTS[@]}"}") 2>&1 | tee "$TEST_LOG"
	pipe_status=("${PIPESTATUS[@]}")
	set -e
	test_status=${pipe_status[0]}
	[ "${pipe_status[1]}" = 0 ] ||
		die "could not write $TEST_LOG"

	# 'perf test' exits 0 even when tests fail or time out, only the
	# summary tells.
	if [ "$test_status" = 0 ] &&
	   grep -qE '^(Failed|Timed out) tests +: +[1-9]' "$TEST_LOG"; then
		test_status=1
	fi

	grep -E '^(Passed|Skipped|Failed|Timed out) .*:' "$TEST_LOG" || true
	[ "$test_status" = 0 ] ||
		warn "some tests failed, coverage is still collected"
}

merge_profiles()
{
	local count size

	# llvm-profdata reads the list from a file: a full run leaves
	# thousands of profiles, too many for the command line.
	find "$COV_DIR/raw" -name '*.profraw' | sort > "$COV_DIR/profiles.txt"
	count=$(wc -l < "$COV_DIR/profiles.txt")
	size=$(du -sh "$COV_DIR/raw" 2>/dev/null | cut -f1)
	[ "$count" -gt 0 ] ||
		die "no .profraw files, was perf rebuilt with -fprofile-instr-generate?"

	say "Merging $count profile files ($size)"
	if ! "$LLVM_PROFDATA" merge -sparse \
			--input-files="$COV_DIR/profiles.txt" \
			-o "$PROFDATA" 2> "$COV_DIR/merge.log"; then
		# A profile left half written, by a perf killed at exit or by
		# a full tmpfs, makes llvm-profdata drop the whole merge:
		# retry without the invalid ones.
		warn "some profile files are unusable, merging the rest"
		"$LLVM_PROFDATA" merge -sparse --failure-mode=all \
			--input-files="$COV_DIR/profiles.txt" -o "$PROFDATA" \
			2> "$COV_DIR/merge.log" ||
			die "llvm-profdata merge failed, see $COV_DIR/merge.log"
	fi
}

llvm_cov_objects()
{
	echo "$BUILD_DIR/perf"
	local so

	# These are dlopened by perf and have to be profiled separately;
	# libperf is linked into perf as a static library.
	for so in libperf-gtk.so libperf-jvmti.so; do
		[ -f "$BUILD_DIR/$so" ] && echo "--object=$BUILD_DIR/$so"
	done
	for so in "$BUILD_DIR"/dlfilters/*.so; do
		[ -f "$so" ] && echo "--object=$so"
	done
	for so in "$BUILD_DIR"/python/perf*.so; do
		[ -f "$so" ] && echo "--object=$so"
	done
}

render_report()
{
	local objects

	mapfile -t objects < <(llvm_cov_objects)
	say "Generating report"
	"$LLVM_COV" report "${objects[@]}" --instr-profile="$PROFDATA" \
		> "$REPORT" || die "llvm-cov report failed"
}

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

total_cov()
{
	awk '$1 == "TOTAL" { print $8, $9, $10 }' "$1"
}

total_cov_perf()
{
	awk -v build="$BUILD_AWK_DIR" "$AWK_PERF"'
		generated($1) { next }
		$1 != "TOTAL" &&
		($1 ~ /(^|\/)tools\/perf\// || $1 ~ /^perf\//) {
			lines += $8
			missed += $9
		}
		END {
			if (lines)
				printf "%d %d %.2f%%\n", lines, missed,
					100 * (lines - missed) / lines
		}' "$1"
}

since_baseline()
{
	local old cur

	[ "$USE_BASELINE" = 1 ] && [ -f "$BASELINE" ] &&
		! cmp -s "$BASELINE" "$REPORT" || return 0
	old=$(awk '$1 == "TOTAL" { print $10 }' "$BASELINE")
	cur=$(awk '$1 == "TOTAL" { print $10 }' "$REPORT")
	[ -n "$old" ] && [ -n "$cur" ] || return 0
	awk -v old="$old" -v cur="$cur" 'BEGIN {
		sub(/%$/, "", cur)
		printf "  (previous %s, %+.2f%%)", old, cur - old }'
}

print_summary()
{
	local lines missed cover p_lines p_missed p_cover

	read -r lines missed cover < <(total_cov "$REPORT") ||
		die "no TOTAL row in $REPORT"
	printf "\n\033[1mTotal:\033[0m %s of %s lines covered (%s)" \
		"$((lines - missed))" "$lines" "$cover"
	since_baseline
	printf "\n"

	read -r p_lines p_missed p_cover < <(total_cov_perf "$REPORT") || true
	if [ -n "$p_lines" ]; then
		printf "  perf: %s of %s lines covered (%s)\n" \
			"$((p_lines - p_missed))" "$p_lines" "$p_cover"
	fi
}

update_baseline()
{
	[ "$USE_BASELINE" = 1 ] || return 0
	# A -t run covers a subset, saving it would poison the next run.
	if [ ${#TESTS[@]} -gt 0 ]; then
		printf "\nBaseline not updated, this was a subset run\n"
		return 0
	fi
	[ -f "$BASELINE" ] && mv "$BASELINE" "$BASELINE.prev"
	cp "$REPORT" "$BASELINE"
	printf "\nBaseline saved: %s\n" "$BASELINE"
}

if [ "$PREP" = 0 ]; then
	command -v "$LLVM_COV" > /dev/null ||
		die "$LLVM_COV not found (set LLVM_COV)"
	command -v "$LLVM_PROFDATA" > /dev/null ||
		die "$LLVM_PROFDATA not found (set LLVM_PROFDATA)"
fi

if [ "$SKIP_BUILD" = 0 ]; then
	command -v clang > /dev/null ||
		die "clang not found, it is the only compiler with -fcoverage-mapping"
fi

# Root does not rebuild a prepared build: use the perf already in
# BUILD_DIR, or build when there is none.
if [ "$ROOT" = 1 ] && [ "$PREP" = 0 ]; then
	if [ -x "$BUILD_DIR/perf" ]; then
		SKIP_BUILD=1
		say "Running as root, using the coverage build in $BUILD_DIR"
	else
		say "Running as root, building perf in $BUILD_DIR"
	fi
fi

test_status=0

if [ "$PREP" = 1 ]; then
	build_perf
	say "Coverage build ready: $BUILD_DIR/perf"
	make_hint="make -C $PERF_DIR coverage"
	[ "$BUILD_DIR" = "$PERF_DIR" ] || make_hint+=" O=$BUILD_DIR"
	printf "As root: %s\n" "$make_hint"
	exit 0
fi

build_perf
run_tests
merge_profiles

[ -f "$PROFDATA" ] || die "no $PROFDATA, the merge did not run"

export LC_ALL=C
render_report
print_summary
update_baseline

printf "\nReport:  %s\n" "$REPORT"
printf "Table:   %s/scripts/coverage-table.sh\n" "$PERF_DIR"
exit "$test_status"
