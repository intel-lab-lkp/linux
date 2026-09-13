#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# perf-stuck - tell a spinning perf apart from a blocked or recursing one
#
# Arnaldo Carvalho de Melo <acme@redhat.com>
#
# PROTOTYPE: this wants to become a first class 'perf stuck' command, that
# samples a running perf, or any other process, from inside perf, with the
# knowledge of the phases perf goes through and of the DWARF type chasing
# loops built in, instead of this shell script poking at /proc and shelling
# out to gdb.  It is here as a stopgap, to be able to tell where a perf is
# stuck while looking at hangs such as the one 'perf report -s type' hits
# on dwz compressed debug info.
#
# Samples /proc/<pid> at a fixed interval and prints, for each sample:
#
#   the CPU time used since the previous sample, so a process burning a
#   full interval's worth of ticks is spinning, while one using none is
#   blocked
#
#   the [stack] mapping start, which moves down as the stack grows, the
#   giveaway for runaway recursion, together with its size
#
#   the last line of a progress log, when one is given, e.g. the stderr
#   of 'perf report --progress', to see which phase is stuck
#
# A process that burns CPU with a constant stack and no progress is in an
# unbounded loop, e.g. a die_get_pointer_type() chain that got into a
# cycle, while one whose [stack] start keeps moving down is recursing.
#
# With -g it runs gdb, using the perf-stuck.gdb that sits next to this
# script, when no progress is made for two consecutive samples, which for
# a perf in a DWARF type chasing loop prints the DIE chain it is walking.
#
# usage: perf-stuck.sh [options] <pid|process-name>

set -u

usage() {
	cat <<-EOF
	usage: perf-stuck.sh [options] <pid|process-name>

	  -i <secs>   sampling interval (default: 10)
	  -n <count>  stop after this many samples (default: watch till it exits)
	  -l <file>   progress log, its last line is printed with every sample
	  -g          run gdb with perf-stuck.gdb when no progress is made for
	              two consecutive samples, writing the output to a temp file
	  -x <file>   use this gdb command file instead of perf-stuck.gdb
	  -h          this help
	EOF
	exit "${1:-0}"
}

interval=10
count=0
progress_log=
use_gdb=
gdb_cmds=

while getopts "i:n:l:gx:h" opt; do
	case "$opt" in
	i) interval=$OPTARG ;;
	n) count=$OPTARG ;;
	l) progress_log=$OPTARG ;;
	g) use_gdb=1 ;;
	x) gdb_cmds=$OPTARG ;;
	h) usage 0 ;;
	*) usage 1 ;;
	esac
done
shift $((OPTIND - 1))

[ $# -eq 1 ] || usage 1

if [[ "$1" =~ ^[0-9]+$ ]]; then
	pid=$1
else
	pid=$(pgrep -x "$1" | head -1)
	[ -n "$pid" ] || { echo "no process named '$1'"; exit 1; }
fi

[ -d /proc/"$pid" ] || { echo "no process $pid"; exit 1; }

if [ -n "$use_gdb" ] && [ -z "$gdb_cmds" ]; then
	gdb_cmds=$(dirname "$0")/perf-stuck.gdb
	[ -r "$gdb_cmds" ] || { echo "cannot read $gdb_cmds"; exit 1; }
fi

hz=$(getconf CLK_TCK)
psz=$(getconf PAGESIZE)
prev_cpu=
prev_stack=
prev_progress=
stuck=0
gdb_done=
nsample=0

echo "watching $pid ($(tr '\0' ' ' < /proc/"$pid"/cmdline)) every ${interval}s"

while :; do
	if [ ! -d /proc/"$pid" ]; then
		echo "$(date +%T) process gone"
		break
	fi

	stat=($(awk '{print $3, $14 + $15, $24}' /proc/"$pid"/stat))
	state=${stat[0]}
	cpu=${stat[1]}
	# field 24 is the resident set size in pages
	rss=$(( stat[2] * psz / 1024 ))

	stack=$(awk '/\[stack\]/{print $1; exit}' /proc/"$pid"/maps)
	if [ -n "$stack" ]; then
		stack_start=0x${stack%-*}
		stack_size=$(( 0x${stack#*-} - stack_start ))
		stack_txt="$stack size=$((stack_size / 1024))kB"
	else
		stack_start=
		stack_txt="-"
	fi

	progress=
	[ -n "$progress_log" ] && [ -s "$progress_log" ] && progress=$(tail -1 "$progress_log")

	if [ -n "$prev_cpu" ]; then
		cpu_delta=$(( cpu - prev_cpu ))
		# With a progress log, count the samples that show no progress,
		# without one there is no progress to look at, so count them all:
		# -g then looks at where the process is after two intervals.
		if [ -z "$progress_log" ] ||
		   { [ -n "$progress" ] && [ "$progress" = "$prev_progress" ]; }; then
			stuck=$((stuck + 1))
		else
			stuck=0
		fi
		stuck_txt="stuck=${stuck}"
		[ "$stack_start" != "$prev_stack" ] && stuck_txt="$stuck_txt STACK"
	else
		cpu_delta=0
		stuck_txt=""
	fi

	printf '%s state=%s cpu=+%d (%d.%02ds) rss=%dkB stack=%s %s %s\n' \
	       "$(date +%T)" "$state" "$cpu_delta" \
	       $(( cpu_delta / hz )) $(( (cpu_delta % hz) * 100 / hz )) \
	       "$rss" "$stack_txt" "$stuck_txt" "${progress:-(no progress log)}"

	if [ -n "$use_gdb" ] && [ -z "$gdb_done" ] && [ "$stuck" -ge 2 ]; then
		gdb_log=$(mktemp /tmp/perf-stuck-gdb.XXXXXX)
		gdb -p "$pid" -batch -x "$gdb_cmds" -ex bt \
		    -ex 'perf-die-chain-all' -ex perf-dso -ex detach > "$gdb_log" 2>&1
		gdb_done=1
		echo "... gdb output of $pid in $gdb_log"
	fi

	prev_cpu=$cpu
	prev_stack=$stack_start
	prev_progress=$progress

	nsample=$((nsample + 1))
	[ "$count" -gt 0 ] && [ "$nsample" -ge "$count" ] && break

	sleep "$interval"
done
