#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# perf-stuck - tell a spinning perf apart from a blocked or recursing one
#
# Arnaldo Carvalho de Melo <acme@redhat.com>
#
# PROTOTYPE: wants to become a first class 'perf stuck' command, sampling
# a process from inside perf, with the knowledge of perf's phases and of
# the DWARF type chasing loops built in, instead of poking /proc and
# shelling out to gdb.
#
# Samples /proc/<pid> at a fixed interval and prints the CPU time used
# since the previous sample, the [stack] mapping start and size, and the
# last line of a progress log when one is given, e.g. the stderr of
# 'perf report --progress': burning a full interval with a constant
# stack is a loop, a [stack] start moving down is runaway recursion.
#
# With -g it runs gdb (perf-stuck.gdb) when no progress is made for two
# consecutive samples, printing the DIE chain a DWARF type chase is
# walking.
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

[[ "$interval" =~ ^[0-9]+$ ]] && [ "$interval" -gt 0 ] ||
	{ echo "-i wants a positive integer, got '$interval'"; usage 1; }
[[ "$count" =~ ^[0-9]+$ ]] ||
	{ echo "-n wants a non-negative integer, got '$count'"; usage 1; }
[ -n "$gdb_cmds" ] && [ -z "$use_gdb" ] && { echo "-x needs -g"; usage 1; }

[ $# -eq 1 ] || usage 1

if [[ "$1" =~ ^[0-9]+$ ]]; then
	pid=$1
else
	# Resolve the name against the caller's own processes: as root,
	# unscoped pgrep picks the first match of any user, e.g. one planted
	# to get gdb attached to it, use an explicit pid to look at a perf of
	# another user.
	pid=$(pgrep -x -u "$(id -u)" -- "$1" | head -1)
	[ -n "$pid" ] || { echo "no process named '$1' owned by $(id -un)"; exit 1; }
fi

[ -d /proc/"$pid" ] || { echo "no process $pid"; exit 1; }

if [ -n "$use_gdb" ]; then
	# Fail before watching, not two samples in when -g would fire.
	command -v gdb > /dev/null || { echo "gdb not found, -g needs it"; exit 1; }
	[ -z "$gdb_cmds" ] && gdb_cmds=$(dirname "$0")/perf-stuck.gdb
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

# The command line is whatever the process was started with, so drop the
# control characters from it: a process started with escape sequences in
# its arguments, e.g. one replaying a log line, would otherwise get them
# replayed on the terminal of whoever runs this.
cmdline=$(tr '\0' ' ' < /proc/"$pid"/cmdline | tr -d '[:cntrl:]')

echo "watching $pid ($cmdline) every ${interval}s"

while :; do
	if [ ! -d /proc/"$pid" ]; then
		echo "$(date +%T) process gone"
		break
	fi

	# Field 2, the command name, is in parentheses and can contain
	# spaces, so drop it together with the pid before splitting so the
	# fields line up.  %d keeps the CPU time out of scientific
	# notation, that bash arithmetic can't parse past six digits.
	if ! stat_line=$(awk '{ sub(/^[^ ]+ \(.*\) /, "");
			       printf "%s %d %d\n", $1, $12 + $13, $22 }' \
			 /proc/"$pid"/stat 2>/dev/null); then
		echo "$(date +%T) process gone"
		break
	fi

	# The process can be gone between the check above and this read, in
	# which case there is nothing to report: 'set -u' would otherwise
	# turn the unbound fields into an aborted script.
	if [ -z "$stat_line" ]; then
		echo "$(date +%T) process gone"
		break
	fi

	stat=($stat_line)
	state=${stat[0]}
	cpu=${stat[1]}
	# field 24 of /proc/<pid>/stat, the resident set size in pages
	rss=$(( stat[2] * psz / 1024 ))

	stack=$(awk '/\[stack\]/{print $1; exit}' /proc/"$pid"/maps 2>/dev/null)
	if [ -n "$stack" ]; then
		stack_start=0x${stack%-*}
		stack_size=$(( 0x${stack#*-} - stack_start ))
		stack_txt="$stack size=$((stack_size / 1024))kB"
	else
		stack_start=
		stack_txt="-"
	fi

	progress=
	[ -n "$progress_log" ] && [ -s "$progress_log" ] && progress=$(tail -1 -- "$progress_log")

	if [ -n "$prev_cpu" ]; then
		cpu_delta=$(( cpu - prev_cpu ))
		# No progress log, or one with nothing written to it yet, leaves
		# no progress to look at, so count the samples that show none:
		# -g then looks at where the process is after two intervals.
		# Otherwise count the repeats of the same last line.
		if [ -z "$progress" ] || [ "$progress" = "$prev_progress" ]; then
			stuck=$((stuck + 1))
		else
			stuck=0
			gdb_done=
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
		# The gdb script makes inferior calls, and a call into a perf
		# wedged in a loop never returns, so bound the run; SIGINT
		# releases the inferior instead of leaving perf stopped.
		timeout --signal=INT 30 gdb -p "$pid" -batch -x "$gdb_cmds" -ex bt \
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
