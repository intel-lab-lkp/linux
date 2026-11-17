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
# skips the build and goes straight to the tests, unless --build is
# given.  The sampling based tests move the percentages by a percent or
# two between identical runs.

set -euo pipefail

PERF_DIR=$(cd "$(dirname "$0")/.." && pwd -P)
TOOLS_DIR=$(cd "$PERF_DIR/.." && pwd -P)
SRC_DIR=$(cd "$TOOLS_DIR/.." && pwd -P)

# perf links objects from these directories, so a change in any of them
# makes a build and a profile stale.
PERF_SOURCES=("$PERF_DIR" "$TOOLS_DIR/lib/perf" "$TOOLS_DIR/lib/api"
	      "$TOOLS_DIR/lib/subcmd" "$TOOLS_DIR/lib/bpf"
	      "$TOOLS_DIR/include")

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
FORCE_BUILD=0
REPORT_ONLY=0
PREP=0
TABLE=0
HTML=0
USE_BASELINE=1
BASELINE=
BASELINE_SET=0
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
  -t, --tests "A;B"      test name fragments, ';' separated (default: all);
                         a subset run does not update the baseline
  -f, --focus FILE       add a file to the per-file table (repeatable)
      --top N            when no -f is given, show the N files with the
                         most uncovered lines (default: $TOP, 0 to disable)
      --baseline FILE    compare against FILE, keeping it read-only
                         (default: COV_DIR/baseline.txt, which a full run
                         saves for the next one)
      --no-baseline      do not compare against or update a baseline
      --uncovered N      uncovered line numbers per focus file (default: $UNCOVERED)
      --skip-build       reuse the existing build, just run the tests
      --build            build as root even when a prepared build exists
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

As root, without --prep or --build, the script uses the build prepared
by 'make coverage-prep' when there is one, and builds otherwise.  The
prepared build is recorded in tools/perf/.coverage-build-dir.

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
	--*=*) optval=${arg#*=}; arg=${arg%%=*}
	       [ -n "$optval" ] || die "$arg needs a value" ;;
	esac

	case "$arg" in
	-b|--build-dir)   BUILD_DIR=${optval:-${2?missing value}}; BUILD_DIR_SET=1
	                  [ -z "$optval" ] && shift ;;
	-c|--cov-dir)     COV_DIR=${optval:-${2?missing value}}; COV_DIR_SET=1
	                  [ -z "$optval" ] && shift ;;
	-j|--jobs)        JOBS=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	-f|--focus)       FOCUS+=("${optval:-${2?missing value}}"); [ -z "$optval" ] && shift ;;
	--baseline)       BASELINE=${optval:-${2?missing value}}; BASELINE_SET=1
	                  [ -z "$optval" ] && shift ;;
	--uncovered)      UNCOVERED=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	--top)            TOP=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	--dwarf-version)  DWARF_VERSION=${optval:-${2?missing value}}; [ -z "$optval" ] && shift ;;
	-t|--tests)       IFS=';' read -r -a _t <<< "${optval:-${2?missing value}}"
	                  TESTS+=("${_t[@]}"); [ -z "$optval" ] && shift ;;
	--no-baseline)    USE_BASELINE=0 ;;
	--skip-build)     SKIP_BUILD=1 ;;
	--build)          FORCE_BUILD=1; SKIP_BUILD=0 ;;
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
if [ "$BUILD_DIR_SET" = 0 ] && [ "$PREP" = 0 ] &&
   { [ "$ROOT" = 1 ] || [ "$SKIP_BUILD" = 1 ] ||
     [ "$REPORT_ONLY" = 1 ] || [ "$TABLE" = 1 ]; } &&
   [ -f "$PERF_DIR/.coverage-build-dir" ]; then
	read -r BUILD_DIR < "$PERF_DIR/.coverage-build-dir"
fi
[ -n "$BUILD_DIR" ] || BUILD_DIR=$DEFAULT_BUILD_DIR

if [ "$COV_DIR_SET" = 0 ] && [ -f "$BUILD_DIR/cov/state" ]; then
	COV_DIR=$(awk -F= '$1 == "COV_DIR" { print $2; exit }' \
		"$BUILD_DIR/cov/state")
fi
[ -n "$COV_DIR" ] || COV_DIR=$BUILD_DIR/cov

# The tests run from the kernel tree root, so keep these absolute.  The
# Makefile passes FULL_O resolved with readlink -f; resolve here too so an
# in-tree build directory compares equal to the physical PERF_DIR below.
abspath()
{
	realpath -m "$1"
}
BUILD_DIR=$(abspath "$BUILD_DIR")
COV_DIR=$(abspath "$COV_DIR")

# llvm-cov strips the prefix all reported files share.  A build inside
# tools/perf makes the rows read perf/<build>/..., so BUILD_REL lets the
# report leave those generated files out; it is empty for any other layout.
BUILD_REL=
case "$BUILD_DIR" in
"$PERF_DIR"/*) BUILD_REL=${BUILD_DIR#"$PERF_DIR"/} ;;
esac

[ -n "$BASELINE" ] || BASELINE=$COV_DIR/baseline.txt
[[ $TOP =~ ^[0-9]+$ ]] || die "--top wants a number, not '$TOP'"
[[ $MIN_LINES =~ ^[0-9]+$ ]] || die "MIN_LINES wants a number, not '$MIN_LINES'"

# Root does not rebuild a prepared build unless asked: use the perf
# prepared by coverage-prep, or build when there is none.
if [ "$ROOT" = 1 ] && [ "$PREP" = 0 ] && [ "$TABLE" = 0 ]; then
	if [ "$REPORT_ONLY" = 0 ] && [ "$SKIP_BUILD" = 0 ] &&
	   { [ "$FORCE_BUILD" = 1 ] ||
	     { [ ! -f "$PERF_DIR/.coverage-build-dir" ] &&
	       [ ! -x "$BUILD_DIR/perf" ]; }; }; then
		say "Running as root, building perf in $BUILD_DIR"
	else
		SKIP_BUILD=1
		say "Running as root, using the coverage build in $BUILD_DIR"
	fi
fi

#
# Under sudo the build directory comes from a file the invoking user wrote;
# root would execute the perf found there and delete and rewrite its cov/
# directory, so refuse one owned by somebody else or writable by another
# group or by others.  Every parent is checked too: a directory another
# user can write lets them swap an entry between this check and the run.
#
check_root_path()
{
	local path=$1 uid gid mode
	[ "$ROOT" = 1 ] && [ -n "${SUDO_UID:-}" ] || return 0
	# abspath() resolves links such as /home -> /var/home on Silverblue
	# and CoreOS, so the walk below checks their targets.  A path not
	# resolved that way, like $BUILD_DIR/perf, must not be a link.
	[ -L "$path" ] && die "$path is a symbolic link, use its target"
	while :; do
		if [ -e "$path" ]; then
			if ! read -r uid gid mode < \
				<(stat -c '%u %g %a' "$path" 2>/dev/null); then
				die "could not stat $path"
			fi
			# A sticky directory (/tmp) lets anybody create entries
			# but not replace one that belongs to somebody else.
			if [ $(( 8#$mode & 8#1000 )) = 0 ]; then
				[ "$uid" = 0 ] || [ "$uid" = "$SUDO_UID" ] ||
					die "$path is owned by uid $uid, not by root or $SUDO_UID"
				# A user private group with umask 002
				# gives 775, so only the developer's
				# own group may write here.
				[ $(( 8#$mode & 8#002 )) = 0 ] ||
					die "$path is writable by others"
				if [ $(( 8#$mode & 8#020 )) != 0 ] &&
				   [ "$gid" != "${SUDO_GID:-}" ]; then
					die "$path is writable by group $gid"
				fi
			fi
		fi
		[ "$path" = / ] && return 0
		path=$(dirname "$path")
	done
}

check_root_path "$BUILD_DIR"
check_root_path "$BUILD_DIR/perf"
check_root_path "$COV_DIR"

#
# A root run leaves the files it writes owned by root; give them back to the
# developer, or the next non-root run cannot clean raw/ or move the baseline.
# The uid comes from sudo, or from the kernel tree when root runs in a
# container with the tree mounted from the host.  Only the directories this
# run created are chowned recursively: a mistyped --cov-dir or --build-dir
# must not hand a shared or system tree to the developer.
#
COV_WRITES=(raw html build.log tests.log report.txt perf.profdata profiles.txt
	    merge.log baseline.txt baseline.txt.prev)

fix_ownership()
{
	local uid gid path
	[ "$ROOT" = 1 ] || return 0
	if [ -n "${SUDO_UID:-}" ]; then
		uid=$SUDO_UID
		gid=${SUDO_GID:-$SUDO_UID}
	else
		read -r uid gid < <(stat -c '%u %g' "$PERF_DIR" 2>/dev/null) || return 0
	fi
	[ "$uid" != 0 ] || return 0

	for path in "${COV_WRITES[@]}"; do
		[ -e "$COV_DIR/$path" ] || continue
		chown -R "$uid:$gid" "$COV_DIR/$path" ||
			warn "could not give $COV_DIR/$path back to uid $uid"
	done
	if [ "$COV_CREATED" = 1 ]; then
		chown "$uid:$gid" "$COV_DIR" ||
			warn "could not give $COV_DIR back to uid $uid"
	fi
	if [ "$BUILD_CREATED" = 1 ]; then
		chown -R "$uid:$gid" "$BUILD_DIR" ||
			warn "could not give $BUILD_DIR back to uid $uid"
	fi
	if [ -n "$BUILD_STAMP" ] && [ -f "$BUILD_STAMP" ]; then
		# -h: find matches a symlink by its own owner, but chown would
		# otherwise follow it and change a file outside the build tree.
		find "$BUILD_DIR" -user 0 -newer "$BUILD_STAMP" \
			-exec chown -h "$uid:$gid" {} + ||
			warn "could not give the files root built in $BUILD_DIR back to uid $uid"
		rm -f "$BUILD_STAMP" || true
	fi
	if [ "$STATE_DIR_CREATED" = 1 ] && [ -d "$STATE_DIR" ]; then
		chown "$uid:$gid" "$STATE_DIR" ||
			warn "could not give $STATE_DIR back to uid $uid"
	fi
	if [ -f "$STATE_FILE" ]; then
		chown "$uid:$gid" "$STATE_FILE" ||
			warn "could not give $STATE_FILE back to uid $uid"
	fi
	if [ -f "$POINTER" ]; then
		chown "$uid:$gid" "$POINTER" ||
			warn "could not give $POINTER back to uid $uid"
	fi
}

BUILD_LOG=$COV_DIR/build.log
TEST_LOG=$COV_DIR/tests.log
REPORT=$COV_DIR/report.txt
PROFDATA=$COV_DIR/perf.profdata
# Discovery point for a later root or --table run, so it stays under
# BUILD_DIR even when --cov-dir points elsewhere.
STATE_FILE=$BUILD_DIR/cov/state
STATE_DIR=$(dirname "$STATE_FILE")
POINTER=$PERF_DIR/.coverage-build-dir

# What exists before this run writes anything: fix_ownership() chowns a
# shared COV_DIR or BUILD_DIR only when root created it.
COV_CREATED=0
[ -d "$COV_DIR" ] || COV_CREATED=1
BUILD_CREATED=0
[ -d "$BUILD_DIR" ] || BUILD_CREATED=1
STATE_DIR_CREATED=0
[ -d "$STATE_DIR" ] || STATE_DIR_CREATED=1
# An existing build directory is not chowned as a whole: a stamp taken
# when the build starts marks the files a root build writes.
BUILD_STAMP=

#
# Sanity checks
#
for src in "${PERF_SOURCES[@]}"; do
	[ -d "$src" ] || die "$src not found"
done
case "$BUILD_DIR" in
"$PERF_DIR"|"$TOOLS_DIR"|"$SRC_DIR")
	die "$BUILD_DIR would mix generated files with the sources, use -b DIR" ;;
esac

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
		die "no $BUILD_DIR/perf, run 'make -C $PERF_DIR O=$BUILD_DIR" \
		    "coverage-prep' first, or pass --build"
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
	mkdir -p "$STATE_DIR" ||
		die "could not create $STATE_DIR"
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

#
# A source file newer than $1, if any, skipping the build directory.
#
newer_sources()
{
	find "${PERF_SOURCES[@]}" -path "$BUILD_DIR" -prune -o \
		-name '*.[ch]' -newer "$1" -print -quit 2>/dev/null || true
}

check_build_freshness()
{
	local newer
	[ -x "$BUILD_DIR/perf" ] || return 0
	newer=$(newer_sources "$BUILD_DIR/perf")
	[ -z "$newer" ] ||
		warn "build is older than source changes" \
		     "(e.g. ${newer#"$SRC_DIR"/}), rerun coverage-prep"
}

build_perf()
{
	if [ "$SKIP_BUILD" = 1 ]; then
		check_coverage_build
		check_build_freshness
		return
	fi

	say "Building perf in $BUILD_DIR"
	mkdir -p "$BUILD_DIR"
	if [ "$ROOT" = 1 ] && [ "$BUILD_CREATED" = 0 ]; then
		BUILD_STAMP=$BUILD_DIR/.coverage-stamp
		: > "$BUILD_STAMP" 2>/dev/null ||
			{ warn "could not write $BUILD_STAMP"; BUILD_STAMP=; }
	fi
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
	(cd "$SRC_DIR" &&
		PATH="$BUILD_DIR:$PATH" \
		LLVM_PROFILE_FILE="$COV_DIR/raw/perf-%p-%m.profraw" \
			"$BUILD_DIR/perf" test -v \
				"${TESTS[@]+"${TESTS[@]}"}") 2>&1 | tee "$TEST_LOG"
	test_status=${PIPESTATUS[0]}
	set -e

	# 'perf test' exits 0 even when tests fail or time out, only the
	# summary tells.
	if [ "$test_status" = 0 ] &&
	   grep -qE '^(Failed|Timed out) tests +: +[1-9]' "$TEST_LOG"; then
		test_status=1
	fi

	grep -E '^(Passed|Skipped|Failed|Timed out) .*:' "$TEST_LOG" || true
	[ "$test_status" = 0 ] ||
		warn "some tests failed, coverage is still collected"
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
	local count size unusable=0 list=$COV_DIR/profiles.txt
	# A full run leaves thousands of profiles: llvm-profdata reads the
	# list from a file so they do not have to fit on the command line.
	find "$COV_DIR/raw" -name '*.profraw' | sort > "$list"
	count=$(wc -l < "$list")
	size=$(du -sh "$COV_DIR/raw" 2>/dev/null | cut -f1)
	[ "$count" -gt 0 ] ||
		die "no .profraw files, was perf rebuilt with -fprofile-instr-generate?"

	say "Merging $count profile files ($size)"
	if ! "$LLVM_PROFDATA" merge -sparse --input-files="$list" \
			-o "$PROFDATA" 2> "$COV_DIR/merge.log"; then
		# A file left half written, by a perf killed at exit or by a full
		# tmpfs, makes llvm-profdata drop the whole merge and with it the
		# run's measurement: retry keeping what can be read and say how
		# much was lost.  --failure-mode=warn exists since LLVM 14, the
		# oldest llvm-profdata the test images ship.
		warn "some profile files are unusable, merging the rest"
		"$LLVM_PROFDATA" merge -sparse --failure-mode=warn \
			--input-files="$list" -o "$PROFDATA" \
			2> "$COV_DIR/merge.log" ||
			die "llvm-profdata merge failed, see $COV_DIR/merge.log"
		# Count only warnings naming a profile: the merge also warns
		# about counter and hash mismatches.
		unusable=$(sed -n 's/^warning: \(.*\.profraw\):.*/\1/p' \
			"$COV_DIR/merge.log" | sort -u | wc -l)
		[ "$unusable" -lt "$count" ] ||
			die "no usable profile among $count, see $COV_DIR/merge.log"
		warn "merged $((count - unusable)) profile files, ignored $unusable unusable"
	fi
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
# The report parsers share these: llvm-cov strips the prefix every
# reported file has in common, so a tools/perf source can read as
# perf/util/evsel.c; perf_relpath() gives the path -f takes, generated()
# spots a build directory row.
AWK_PERF='
function perf_relpath(p) {
	sub(/^.*tools\/perf\//, "", p)
	sub(/^perf\//, "", p)
	return p
}
function generated(p) {
	return build != "" && index(perf_relpath(p), build "/") == 1
}
'

# Line coverage of a full report for one file: "lines missed cover fn%".
# Match the requested path as a suffix: a build outside the source tree
# gives ".../tools/perf/...", one inside it "tools/perf/...", and one
# inside tools/ makes llvm-cov strip the "<srctree>/tools/" prefix shared
# by every reported file, giving "perf/...".
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
	awk -v f="$2" "$AWK_PERF"'
		{
			g = perf_relpath(f)
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

total_cov_perf()
{
	# Generated files under an in-tree build directory read as
	# perf/<build>/...; leave them out so an in and an out of tree
	# build count the same files.
	awk -v build="$BUILD_REL" "$AWK_PERF"'
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

# Delta between the totals the $3 helper (total_cov or total_cov_perf)
# prints for $prev and $cur.
total_delta()
{
	local prev=$1 cur=$2 total=$3 old cur_cov
	[ -f "$prev" ] || return 0
	old=$("$total" "$prev" | awk '{print $3}')
	cur_cov=$("$total" "$cur" | awk '{print $3}')
	[ -n "$old" ] && [ -n "$cur_cov" ] || return 0
	awk -v cur="$cur_cov" -v old="$old" 'BEGIN {
		sub(/%$/, "", cur); sub(/%$/, "", old)
		printf "%+.2f%%", cur - old }'
}

#
# "  (previous X, +Y)": the same total in $BASELINE and its delta, when
# there is a baseline from an earlier run.
#
since_baseline()
{
	local prev delta
	[ "$USE_BASELINE" = 1 ] && [ -f "$BASELINE" ] &&
		! cmp -s "$BASELINE" "$REPORT" || return 0
	prev=$("$1" "$BASELINE" | awk '{print $3}')
	delta=$(total_delta "$BASELINE" "$REPORT" "$1")
	[ -n "$prev" ] && [ -n "$delta" ] || return 0
	printf "  (previous %s, %s)" "$prev" "$delta"
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
	rows=$(awk -v min="$MIN_LINES" -v build="$BUILD_REL" "$AWK_PERF"'
		($1 ~ /(^|\/)tools\/perf\// || $1 ~ /^perf\//) &&
		!generated($1) && $8 >= min && $9 > 0 {
			printf "%s %s %s %s %s\n", $9, $8, $10, $7,
				perf_relpath($1)
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
	local lines missed cover p_lines p_missed p_cover file cov
	read -r lines missed cover < <(total_cov "$REPORT")
	printf "\n\033[1mTotal:\033[0m %s of %s lines covered (%s)" \
		"$((lines - missed))" "$lines" "$cover"
	since_baseline total_cov
	printf "\n"
	read -r p_lines p_missed p_cover < <(total_cov_perf "$REPORT") || true
	if [ -n "$p_lines" ]; then
		printf "  perf: %s of %s lines covered (%s)" \
			"$((p_lines - p_missed))" "$p_lines" "$p_cover"
		since_baseline total_cov_perf
		printf "\n"
	fi
	printf "\n"

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
	if [ ${#FOCUS[@]} -gt 0 ]; then
		local file
		for file in "${FOCUS[@]}"; do
			table_row "$REPORT" "$file"
		done | sort -t$'\t' -k5,5gr
		return
	fi

	awk -v min="$MIN_LINES" -v build="$BUILD_REL" "$AWK_PERF"'
		($1 ~ /(^|\/)tools\/perf\// || $1 ~ /^perf\//) &&
		!generated($1) && $8 >= min && $9 > 0 {
			printf "%s\t%s\t%d\t%d\t%.2f\t%s\t%d\t%s\n",
			       perf_relpath($1), $8, $8 - $9, $9, $10,
			       $5, $5 - $6, $7
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

	w=5
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
	newer=$(newer_sources "$PROFDATA")
	if [ -n "$newer" ]; then
		warn "profiles are older than source changes" \
		     "(e.g. ${newer#"$SRC_DIR"/}), results may be stale"
	fi
}

update_baseline()
{
	[ "$USE_BASELINE" = 1 ] || return 0
	[ "$REPORT_ONLY" = 1 ] && return 0
	# A --baseline file is only read, the default one rolls forward.
	[ "$BASELINE_SET" = 0 ] || return 0
	# A -t run covers a subset, saving it would poison the next full run.
	if [ ${#TESTS[@]} -gt 0 ]; then
		printf "\nBaseline not updated, this was a subset run\n"
		return 0
	fi
	[ -f "$BASELINE" ] && mv "$BASELINE" "$BASELINE.prev"
	cp "$REPORT" "$BASELINE"
	printf "\nBaseline saved: %s\n" "$BASELINE"
}

test_status=0

if [ "$TABLE" = 1 ]; then
	[ -f "$REPORT" ] || die "no $REPORT, run coverage first"
	# The table code parses llvm-cov's decimal points; keep the numeric
	# locale fixed here only, the tests must run with their normal one.
	export LC_ALL=C
	check_stale
	print_table
	exit 0
fi

if [ "$ROOT" = 1 ]; then
	trap fix_ownership EXIT
fi

if [ "$PREP" = 1 ]; then
	build_perf
	# build_perf() records the build unless it only checked it.
	if [ "$SKIP_BUILD" = 1 ]; then
		write_state
	fi
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

export LC_ALL=C
render_report
print_summary
print_uncovered
save_html
update_baseline

printf "\nReport:  %s\n" "$REPORT"
[ "$HTML" = 1 ] && printf "HTML:    %s\n" "$COV_DIR/html/index.html"

exit "$test_status"
