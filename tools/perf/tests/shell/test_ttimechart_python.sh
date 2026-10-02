#!/bin/bash
# perf timechart --tui (ttimechart.py) test
# SPDX-License-Identifier: GPL-2.0

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/setup_python.sh
. "${shelldir}"/lib/setup_python.sh

if ! perf check feature -q libtraceevent; then
	echo "Skipping test, libtraceevent is disabled"
	exit 2
fi

if ! "$PYTHON" -c 'import perf' > /dev/null 2>&1; then
	echo "Skipping test, perf python module not found"
	exit 2
fi

if ! "$PYTHON" -c 'import textual' > /dev/null 2>&1; then
	echo "Skipping test, python textual library not found"
	exit 2
fi

err=0
perfdata=
output=
clockdata=

cleanup() {
	[ -n "${perfdata}" ] && rm -f "${perfdata}"*
	[ -n "${clockdata}" ] && rm -f "${clockdata}"*
	rm -f "${output}"
	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

perfdata=$(mktemp /tmp/__perf_ttimechart_test.perf.data.XXXXX)
output=$(mktemp /tmp/__perf_ttimechart_test.output.XXXXX)
clockdata=$(mktemp /tmp/__perf_ttimechart_test.clock.data.XXXXX)

workload() {
	for _ in 1 2 3 4 5; do
		sleep 0.01
	done
}

record() {
	# perf timechart record is system wide and so may need privileges,
	# fall back to recording just the workload's scheduler events.
	if perf timechart record -o "${perfdata}" -- \
		bash -c "$(declare -f workload); workload" > /dev/null 2>&1; then
		return 0
	fi
	if perf record -R -c 1 -e sched:sched_switch -e sched:sched_wakeup \
		-o "${perfdata}" -- bash -c "$(declare -f workload); workload" \
		> /dev/null 2>&1; then
		return 0
	fi
	return 1
}

test_plumbing() {
	echo "perf timechart --tui plumbing test"
	# A missing input file should be reported by the script launched
	# through perf script, this doesn't need a terminal.
	if perf timechart --tui -i "${perfdata}.missing" > "${output}" 2>&1; then
		echo "perf timechart --tui plumbing test [Failed: missing file not an error]"
		err=1
		return
	fi
	if ! grep -q "not found" "${output}"; then
		echo "perf timechart --tui plumbing test [Failed: script not launched]"
		cat "${output}"
		err=1
		return
	fi
	if perf timechart --live -i "${perfdata}" > "${output}" 2>&1 ||
	   ! grep -q -- "--live can't be used with -i" "${output}"; then
		echo "perf timechart --tui plumbing test [Failed: --live with -i not rejected]"
		cat "${output}"
		err=1
		return
	fi
	if perf timechart --live --window 0 < /dev/null > "${output}" 2>&1 ||
	   ! grep -q -- "--window must be a positive" "${output}"; then
		echo "perf timechart --tui plumbing test [Failed: --window 0 not rejected]"
		cat "${output}"
		err=1
		return
	fi
	if perf timechart --tui --window 5 < /dev/null > "${output}" 2>&1 ||
	   ! grep -q -- "require --live" "${output}"; then
		echo "perf timechart --tui plumbing test [Failed: --window without --live]"
		cat "${output}"
		err=1
		return
	fi
	if perf timechart --live -I -P < /dev/null > "${output}" 2>&1 ||
	   ! grep -q -- "-I cannot be used with -P or -T" "${output}"; then
		echo "perf timechart --tui plumbing test [Failed: -I with -P not rejected]"
		cat "${output}"
		err=1
		return
	fi
	echo "perf timechart --tui plumbing test [Success]"
}

test_no_events() {
	echo "ttimechart no events test"
	if ! perf record -e task-clock -o "${clockdata}" -- true > /dev/null 2>&1; then
		echo "ttimechart no events test [Skipped: perf record failed]"
		return
	fi
	if perf script ttimechart -i "${clockdata}" --dump > "${output}" 2>&1; then
		echo "ttimechart no events test [Failed: expected an error]"
		err=1
		return
	fi
	if ! grep -q "no scheduler, power or I/O events" "${output}"; then
		echo "ttimechart no events test [Failed: unexpected error message]"
		cat "${output}"
		err=1
		return
	fi
	# The same should be reported when reading the data from a pipe.
	if perf record -e task-clock -o - -- true 2> /dev/null | \
		perf script ttimechart -i - --dump > "${output}" 2>&1; then
		echo "ttimechart no events test [Failed: expected an error from a pipe]"
		err=1
		return
	fi
	if ! grep -q "no scheduler, power or I/O events" "${output}"; then
		echo "ttimechart no events test [Failed: unexpected error message from a pipe]"
		cat "${output}"
		err=1
		return
	fi
	# Exercise LiveRecorder's perf record subprocess, control pipe and
	# -o output file using task-clock when tracepoints may be unavailable.
	rm -f "${clockdata}"
	if ! "$PYTHON" - "${PERF_BIN:-perf}" "${clockdata}" \
		"$(declare -f workload); workload" > "${output}" 2>&1 <<'EOF'
import asyncio
import os
import sys
import ttimechart as tc

async def run() -> None:
    rec = tc.LiveRecorder(sys.argv[1], False, False, ["bash", "-c", sys.argv[3]], sys.argv[2])
    rec.events = lambda: ["task-clock"]
    rec.start()
    app = tc.TimechartApp("live", False, False, [], live=rec, window=5.0)
    try:
        async with app.run_test(size=(120, 40)) as pilot:
            await pilot.press("space")
            await pilot.pause(0.1)
            await pilot.press("space")
            for _ in range(300):
                if app.return_code is not None:
                    break
                await pilot.pause(0.05)
    finally:
        rec.stop()
    if app.return_code != 1 or not app.data.nr_samples or not os.path.getsize(sys.argv[2]):
        raise RuntimeError(f"Unexpected result: rc={app.return_code} samples={app.data.nr_samples}")

asyncio.run(run())
EOF
	then
		echo "ttimechart no events test [Failed: LiveRecorder task-clock test]"
		cat "${output}"
		err=1
		return
	fi
	echo "ttimechart no events test [Success]"
}

test_dump() {
	echo "ttimechart dump test"
	if ! perf script ttimechart -i "${perfdata}" --dump > "${output}" 2>&1; then
		echo "ttimechart dump test [Failed: script failed]"
		cat "${output}"
		err=1
		return
	fi
	if ! grep -q "^Duration: .*sched events: [1-9]" "${output}" ||
	   ! grep -q "^CPU [0-9]*: busy" "${output}" ||
	   ! grep -q "^Task .*Running .*Waiting .*Blocked" "${output}" ||
	   ! grep -q "sleep" "${output}"; then
		echo "ttimechart dump test [Failed: unexpected output]"
		cat "${output}"
		err=1
		return
	fi

	# Tasks only shouldn't show CPUs, power only shouldn't show tasks.
	perf script ttimechart -i "${perfdata}" --dump -T > "${output}" 2>&1 || true
	if grep -q "^CPU [0-9]*: " "${output}" ||
	   ! grep -q "^Task .*Running .*Waiting .*Blocked" "${output}"; then
		echo "ttimechart dump test [Failed: -T output]"
		cat "${output}"
		err=1
		return
	fi
	perf script ttimechart -i "${perfdata}" --dump -P > "${output}" 2>&1 || true
	if grep -q "^Task .*Running .*Waiting .*Blocked" "${output}" ||
	   ! grep -q "^CPU [0-9]*: " "${output}"; then
		echo "ttimechart dump test [Failed: -P output]"
		cat "${output}"
		err=1
		return
	fi

	# Filtering on the workload shouldn't show other tasks.
	perf script ttimechart -i "${perfdata}" --dump -p sleep > "${output}" 2>&1 || true
	if ! grep -q "^sleep " "${output}" || grep -q "^bash " "${output}"; then
		echo "ttimechart dump test [Failed: -p output]"
		cat "${output}"
		err=1
		return
	fi
	echo "ttimechart dump test [Success]"
}

test_headless_ui() {
	echo "ttimechart headless UI test"
	# Drive the textual app without a terminal, exercising the views and
	# key bindings.
	if ! "$PYTHON" - "${perfdata}" > "${output}" 2>&1 <<'EOF'
import asyncio
import sys
import ttimechart as tc

async def run() -> None:
    # The app starts before the data is loaded in a background thread.
    app = tc.TimechartApp(sys.argv[1], False, False, [])
    async with app.run_test(size=(120, 40)) as pilot:
        for _ in range(600):
            if app.loaded:
                break
            await pilot.pause(0.1)
        if not app.loaded:
            raise RuntimeError("Timed out loading data")
        await pilot.pause()
        tabs = app.query_one(tc.TabbedContent)
        for tab in ["cpus", "tasks", "summary"]:
            tabs.active = tab
            await pilot.pause()
            await pilot.press("down", "plus", "plus", "right", "n", "p", "minus",
                              "shift+right", "shift+left", "s", "w", "0")
            await pilot.pause()
        tabs.active = "summary"
        await pilot.pause()
        await pilot.press("enter")
        await pilot.pause()
        details = str(app.query_one("#details", tc.Static).render())
        if "Cursor" not in details:
            raise RuntimeError(f"Unexpected details: {details}")
        print("headless UI ok")

asyncio.run(run())
EOF
	then
		echo "ttimechart headless UI test [Failed: exception]"
		cat "${output}"
		err=1
		return
	fi
	if ! grep -q "headless UI ok" "${output}"; then
		echo "ttimechart headless UI test [Failed: unexpected output]"
		cat "${output}"
		err=1
		return
	fi
	echo "ttimechart headless UI test [Success]"
}

test_live_synthetic() {
	echo "ttimechart live UI test (synthetic)"
	# Exercise live mode's rolling window, pruning, pause/resume and
	# following with a fake recorder, which doesn't need tracepoint
	# permissions.
	if ! "$PYTHON" - > "${output}" 2>&1 <<'EOF'
import asyncio
import threading
import time
from time import monotonic_ns
import ttimechart as tc

SEC = 1_000_000_000

class Ev:
    def __init__(self, name):
        self.name = name
    def __str__(self):
        return f"evsel({self.name})"

class Sample:
    def __init__(self, name, t, cpu, tid, **kw):
        self.evsel = Ev(name)
        self.sample_time = t
        self.sample_cpu = cpu
        self.sample_tid = tid
        self.sample_pid = tid
        self.sample_id = hash(name)
        for k, v in kw.items():
            setattr(self, k, v)

class FakeRecorder:
    def __init__(self):
        self.origin = monotonic_ns()
        self.data_fd = -1
        self.flush_interval = 0.05
        self.paused = False
        self.pause_calls = []
        self.stopped = threading.Event()
        self.finish = False

    def backlog(self):
        return 0.0

    def set_paused(self, paused):
        self.paused = paused
        self.pause_calls.append(paused)

    def stop(self):
        self.stopped.set()

    def error_text(self):
        return ""

async def run() -> None:
    rec = FakeRecorder()

    def fake_read(data, _fd):
        t0 = rec.origin
        # Task 300 only runs at the start so pruning should drop it once the
        # 1s window advances past t0 + 3000.
        data.process_event(Sample("sched:sched_wakeup", t0 + 1000, 0, 1,
                                  pid=300, comm="early", common_pid=1, common_flags=0))
        data.process_event(Sample("sched:sched_switch", t0 + 2000, 0, 0,
                                  prev_pid=0, prev_comm="swapper", prev_state=1,
                                  next_pid=300, next_comm="early"))
        data.process_event(Sample("sched:sched_switch", t0 + 3000, 0, 300,
                                  prev_pid=300, prev_comm="early", prev_state=1,
                                  next_pid=0, next_comm="swapper"))
        cur = 0
        while not rec.stopped.is_set() and not rec.finish:
            now = monotonic_ns()
            if rec.paused:
                time.sleep(0.01)
                continue
            nxt = 100 if cur == 0 else 0
            comm = "worker" if nxt == 100 else "swapper"
            prev_comm = "worker" if cur == 100 else "swapper"
            if nxt:
                data.process_event(Sample("sched:sched_wakeup", now, 0, cur or 1,
                                          pid=nxt, comm=comm, common_pid=cur or 1,
                                          common_flags=0))
            data.process_event(Sample("sched:sched_switch", now, 0, cur,
                                      prev_pid=cur, prev_comm=prev_comm, prev_state=1,
                                      next_pid=nxt, next_comm=comm))
            cur = nxt
            time.sleep(0.005)

    tc.read_events = fake_read
    app = tc.TimechartApp("live", False, False, [], live=rec, window=1.0)
    async with app.run_test(size=(120, 40)) as pilot:
        for _ in range(300):
            with app.data.lock:
                if app.data.pruned_to > rec.origin + 3000:
                    break
            await pilot.pause(0.05)
        with app.data.lock:
            if app.data.pruned_to <= rec.origin + 3000 or 300 in app.data.tasks:
                raise RuntimeError("Task outside the window was not pruned")
            starts = [t.segs.starts[0] for t in app.data.tasks.values() if len(t.segs)]
            if not starts or min(starts) < app.data.pruned_to:
                raise RuntimeError(f"Unpruned segments: {starts} < {app.data.pruned_to}")
        if app.window.last - app.window.first != SEC:
            raise RuntimeError(f"Unexpected window span: {app.window}")

        await pilot.press("space")
        await pilot.pause(0.2)
        if rec.pause_calls != [True] or "Paused" not in app.sub_title:
            raise RuntimeError(f"Did not pause: {rec.pause_calls} {app.sub_title}")
        frozen = app.window.last
        await pilot.pause(0.4)
        if app.window.last != frozen:
            raise RuntimeError("Window moved while paused")
        await pilot.press("space")
        await pilot.pause(0.3)
        if rec.pause_calls != [True, False] or not app.following:
            raise RuntimeError("Did not resume")

        await pilot.press("plus")
        await pilot.pause(0.2)
        if app.following:
            raise RuntimeError("Zooming did not stop following")
        await pilot.press("0")
        await pilot.pause(0.2)
        if not app.following:
            raise RuntimeError("Resetting zoom did not resume following")

        rec.finish = True
        for _ in range(100):
            if not app.recording:
                break
            await pilot.pause(0.05)
        if app.recording or "Finished" not in app.sub_title:
            raise RuntimeError(f"Did not finish: {app.sub_title}")

    # Also test live I/O-only mode (-I).
    io_events = tc.LiveRecorder("perf", False, False, [], None, io_only=True).events()
    if not io_events or not all(e.startswith("syscalls:sys_") for e in io_events):
        raise RuntimeError(f"Unexpected I/O events: {io_events}")
    io_rec = FakeRecorder()

    def fake_io_read(data, _fd):
        now = monotonic_ns()
        # Self I/O should be ignored in live mode.
        own = tc.os.getpid()
        data.process_event(Sample("syscalls:sys_enter_read", now, 0, own, fd=3))
        data.process_event(Sample("syscalls:sys_exit_read", now + 1000, 0, own, ret=64))
        # Workload I/O should be recorded.
        data.process_event(Sample("syscalls:sys_enter_read", now + 2000, 0, 4242, fd=4))
        data.process_event(Sample("syscalls:sys_exit_read", now + 5000, 0, 4242, ret=4096))
        data.process_event(Sample("syscalls:sys_enter_write", now + 6000, 0, 4242, fd=5))
        data.process_event(Sample("syscalls:sys_exit_write", now + 9000, 0, 4242, ret=2048))

    tc.read_events = fake_io_read
    io_app = tc.TimechartApp("live", False, False, [], live=io_rec, window=1.0, io_only=True)
    async with io_app.run_test(size=(120, 40)) as pilot:
        for _ in range(100):
            if not io_app.recording:
                break
            await pilot.pause(0.05)
        await pilot.pause()
        tabs = io_app.query_one(tc.TabbedContent)
        if tabs.active != "io" or not io_app.io_tasks or io_app.data.io_events != 4:
            raise RuntimeError(f"Unexpected I/O state: active={tabs.active} "
                               f"tasks={len(io_app.io_tasks)} events={io_app.data.io_events}")
        tabs.active = "summary"
        await pilot.pause()
        await pilot.press("enter")
        await pilot.pause()
        if tabs.active != "io":
            raise RuntimeError(f"Summary enter did not activate I/O tab: {tabs.active}")
        print("live synthetic ok")

asyncio.run(run())
EOF
	then
		echo "ttimechart live UI test (synthetic) [Failed: exception]"
		cat "${output}"
		err=1
		return
	fi
	if ! grep -q "live synthetic ok" "${output}"; then
		echo "ttimechart live UI test (synthetic) [Failed: unexpected output]"
		cat "${output}"
		err=1
		return
	fi
	echo "ttimechart live UI test (synthetic) [Success]"
}

test_live_record() {
	echo "ttimechart live record test"
	rm -f "${perfdata}.live"
	if ! "$PYTHON" - "${PERF_BIN:-perf}" "${perfdata}.live" \
		"$(declare -f workload); workload" > "${output}" 2>&1 <<'EOF'
import asyncio
import sys
import ttimechart as tc

async def run() -> None:
    rec = tc.LiveRecorder(sys.argv[1], False, False, ["bash", "-c", sys.argv[3]], sys.argv[2])
    rec.start()
    app = tc.TimechartApp("live", False, False, [], live=rec, window=5.0)
    try:
        async with app.run_test(size=(120, 40)) as pilot:
            for _ in range(600):
                if not app.recording:
                    break
                await pilot.pause(0.1)
            if app.recording or not app.data.has_events():
                raise RuntimeError("Live recording of workload did not complete")
            print("live record ok")
    finally:
        rec.stop()

asyncio.run(run())
EOF
	then
		echo "ttimechart live record test [Failed: exception]"
		cat "${output}"
		err=1
		return
	fi
	if ! perf script ttimechart -i "${perfdata}.live" --dump > "${output}" 2>&1 ||
	   ! grep -q "^Duration: .*sched events: [1-9]" "${output}"; then
		echo "ttimechart live record test [Failed: saved perf.data unreadable]"
		cat "${output}"
		err=1
		return
	fi
	echo "ttimechart live record test [Success]"
}

test_plumbing
test_no_events
test_live_synthetic

if ! record; then
	echo "Skipping remaining tests, failed to record scheduler events (permissions?)"
	if [ $err -eq 0 ]; then
		err=2
	fi
	cleanup
	exit $err
fi

test_dump
test_headless_ui
test_live_record

cleanup
exit $err
