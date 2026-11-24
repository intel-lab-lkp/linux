#!/bin/bash
# perf bench tests
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
out=$(mktemp /tmp/__perf_test_bench.out.XXXXX)

cleanup() {
	rm -f "${out}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

# Some benchmarks need root, others only report that the hardware is
# not there, so treat the environment as a skip and a missing result
# as a failure.
bench_skip_reasons="No hardware support|perf_event_open: Permission denied"

run_bench() {
	local name="$1" expect="$2"

	shift 2
	echo "Bench ${name}"
	if ! perf bench "$@" > "${out}" 2>&1; then
		if grep -qE "${bench_skip_reasons}" "${out}"; then
			echo "Bench ${name} [Skipped: not supported here]"
			return
		fi
		echo "Bench ${name} [Failed: perf bench failed]"
		cat "${out}"
		err=1
		return
	fi
	# A benchmark can also decline to run and exit 0, printing that
	# the hardware is not there, as the breakpoint ones do; that is a
	# skip, not a success
	if grep -qE "${bench_skip_reasons}" "${out}"; then
		echo "Bench ${name} [Skipped: not supported here]"
		return
	fi
	if ! grep -qE "${expect}" "${out}"; then
		echo "Bench ${name} [Failed missing '${expect}']"
		cat "${out}"
		err=1
		return
	fi
	echo "Bench ${name} [Success]"
}

run_bench numa/mem "GB/sec" numa mem -p 2 -t 2 -P 2 -T 1 -s 1 -l 1
run_bench numa/mem/options "GB/sec" numa mem -p 1 -t 1 -P 2 -s 1 -l 1 -c -d -r -z -B -R -W
run_bench numa/mem/init "GB/sec" numa mem -p 1 -t 1 -P 2 -s 1 -l 1 -I -H 1 -S -u 1 -B -r -z -Z
if taskset -c 0 true > /dev/null 2>&1; then
	run_bench numa/mem/bind "GB/sec" numa mem -p 2 -t 2 -P 2 -T 1 -s 1 -l 1 -C 0,1 -M 0,0
else
	echo "Bench numa/mem/bind [Skipped: cpu 0 not available]"
fi
run_bench numa/mem/convergence "GB/sec" numa mem -p 1 -t 1 -P 512 -l 1 -s 1 -c -m
run_bench numa/mem/perturb "GB/sec" numa mem -p 1 -t 1 -P 512 -l 1 -s 1 -x 1 -S -d -d
run_bench numa/mem/zero "GB/sec" numa mem -p 1 -t 2 -T 64 -l 1 -s 1 -C 0 -M 0 -B -Z -H -1
run_bench sched/messaging "Total time" sched messaging -g 1 -l 10
run_bench sched/pipe "ops/sec" sched pipe -l 1000
run_bench sched/seccomp-notify "ops/sec" sched seccomp-notify -l 10
run_bench futex/hash "operations/sec" futex hash -t 2 -r 1 -s
run_bench futex/wake "Wokeup" futex wake -t 2
run_bench futex/wake-parallel "Avg per-thread latency" futex wake-parallel -t 2 -w 1
run_bench futex/requeue "Requeued" futex requeue -t 2
run_bench futex/lock-pi "operations/sec" futex lock-pi -t 2 -r 1
run_bench epoll/wait "operations/sec" epoll wait -r 1 -t 2
run_bench epoll/ctl "operations" epoll ctl -r 1 -t 2
run_bench syscall/basic "ops/sec" syscall basic -l 100
run_bench syscall/getpgid "ops/sec" syscall getpgid -l 100
run_bench syscall/fork "ops/sec" syscall fork -l 10
run_bench syscall/execve "ops/sec" syscall execve -l 10
run_bench mem/memcpy "Copying" mem memcpy -s 4KB -l 1
run_bench mem/memset "Copying" mem memset -s 4KB -l 1
run_bench mem/mmap "Copying" mem mmap -s 4KB -l 1
run_bench internals/synthesize "Average data synthesis took" internals synthesize -s -i 1
run_bench internals/synthesize-mt "Average synthesis took" internals synthesize -t -m 1 -M 2 -I 1
run_bench internals/kallsyms-parse "Average kallsyms__parse took" internals kallsyms-parse -i 1
run_bench internals/inject-build-id "Average build-id injection took" internals inject-build-id -i 2 -m 2 -n 2
run_bench internals/pmu-scan "Average PMU scanning took" internals pmu-scan -i 1
run_bench breakpoint/thread "Created/joined" breakpoint thread -t 2 -b 2 -p 1
run_bench breakpoint/enable "Enabled/disabled" breakpoint enable -p 1 -a 1
run_bench uprobe/baseline "usecs/op" uprobe baseline -l 2

if [ "$(id -u)" = 0 ]; then
	run_bench internals/evlist-open-close "Number of events" internals evlist-open-close -i 2
else
	echo "Bench internals/evlist-open-close [Skipped: no root permission]"
fi

# The uprobe programs attach to usleep in libc, which needs BPF and,
# on distributions using musl, is not available by that name.
if perf version --build-options | grep -q 'bpf_skeletons: *\[ on' && [ "$(id -u)" = 0 ]; then
	for mode in empty trace_printk empty_ret trace_printk_ret; do
		echo "Bench uprobe/${mode}"
		if ! perf bench uprobe "${mode}" -l 2 > "${out}" 2>&1; then
			if grep -qE 'Failed to (attach bench uprobe|load and verify BPF skeleton)' "${out}"; then
				echo "Bench uprobe/${mode} [Skipped: no uprobe support]"
				continue
			fi
			echo "Bench uprobe/${mode} [Failed: perf bench failed]"
			cat "${out}"
			err=1
			continue
		fi
		if ! grep -q "usecs/op" "${out}"; then
			echo "Bench uprobe/${mode} [Failed missing result]"
			cat "${out}"
			err=1
			continue
		fi
		echo "Bench uprobe/${mode} [Success]"
	done
else
	echo "Bench uprobe [Skipped: needs root and BPF skeletons]"
fi

cleanup
exit $err
