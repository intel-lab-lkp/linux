#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Verify that rtla removes the tracefs instances it created, even when it is
# interrupted (SIGINT/SIGTERM), killed by its own timeout, or when a shell
# action fails.
#
# This is a *test* for tools/tracing/rtla; it does not change rtla behaviour.
#
# Requires: root, tracefs mounted at /sys/kernel/tracing, CONFIG_OSNOISE_TRACER.
# Usage: rtla_cleanup_test.sh /path/to/rtla

RTLA=${1:-rtla}
TRACEFS=${TRACEFS:-/sys/kernel/tracing}
INSTANCES=$TRACEFS/instances
FAIL=0
PASS=0

log()  { printf '%s\n' "$*"; }
pass() { PASS=$((PASS+1)); log "PASS: $*"; }
fail() { FAIL=$((FAIL+1)); log "FAIL: $*"; }
cleanup_all() {
	for i in osnoise_top osnoise_hist timerlat_top timerlat_hist hwnoise_top; do
		[ -d "$INSTANCES/$i" ] && rmdir "$INSTANCES/$i" 2>/dev/null
	done
}

# Wait up to $2 ms for $1 to disappear.
wait_gone() {
	_path=$1
	_ms=${2:-5000}
	_i=0
	while [ -d "$_path" ] && [ $_i -lt $_ms ]; do
		sleep 0.05
		_i=$((_i+50))
	done
	[ ! -d "$_path" ]
}

# Wait until $1 exists (up to $2 ms).
wait_present() {
	_path=$1
	_ms=${2:-10000}
	_i=0
	while [ ! -d "$_path" ] && [ $_i -lt $_ms ]; do
		sleep 0.05
		_i=$((_i+50))
	done
	[ -d "$_path" ]
}

if [ ! -d "$INSTANCES" ] || [ ! -w "$INSTANCES" ] ||
   [ ! -d "$TRACEFS/osnoise" ]; then
	log "SKIP: tracefs, writable instances or CONFIG_OSNOISE_TRACER unavailable"
	exit 0
fi

# Leave nothing behind from the test before it starts, and make sure the
# command under test is actually usable.
#
# rtla has no --version: main() only knows -h/--help and -usage, and anything
# else falls through to rtla_usage(), which exits 129 after printing the
# version banner. Probing --version therefore always failed and the whole test
# suite silently reported SKIP. Accept 129 as "this binary works".
cleanup_all
if [ ! -x "$RTLA" ]; then
	log "SKIP: $RTLA is not executable"
	exit 0
fi
help_out=$("$RTLA" --help 2>&1)
rtla_rc=$?
if [ "$rtla_rc" -ne 129 ] ||
   ! printf '%s\n' "$help_out" | grep -q 'rtla version'; then
	log "SKIP: $RTLA is not usable (--help exited $rtla_rc, no version banner)"
	exit 0
fi

# Kill a background rtla if a case fails before reaping it.
stop_pid() {
	[ -n "$1" ] || return
	kill -KILL "$1" 2>/dev/null
	wait "$1" 2>/dev/null
}

# Wait until rtla has installed its SIGTERM handler.  The instance directory
# appears before set_signals() runs, so signalling as soon as the directory
# exists races startup; the signal disposition is visible in /proc.
wait_running() {
	_path=$1
	_pid=$2

	wait_present "$_path" 10000 || return 1
	_i=0
	while [ "$_i" -lt 200 ]; do
		_cgt=$(sed -n 's/^SigCgt:[[:space:]]*//p' "/proc/$_pid/status" 2>/dev/null)
		[ -n "$_cgt" ] && [ $((0x$_cgt & 0x4000)) -ne 0 ] && return 0
		kill -0 "$_pid" 2>/dev/null || return 1
		sleep 0.05
		_i=$((_i + 1))
	done
	return 1
}

# ---------------------------------------------------------------------------
# 1. SIGINT during a long run
# ---------------------------------------------------------------------------
cleanup_all
$RTLA osnoise top -q -d 30s -P F:1 >/dev/null 2>&1 &
pid=$!
if ! wait_running "$INSTANCES/osnoise_top" "$pid"; then
	fail "SIGINT: instance was never created (rtla did not start?)"
	stop_pid "$pid"
else
	kill -INT "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	if wait_gone "$INSTANCES/osnoise_top"; then
		pass "SIGINT: instance removed after interrupt"
	else
		fail "SIGINT: instance still present after interrupt"
	fi
fi

# ---------------------------------------------------------------------------
# 2. SIGTERM during a long run
# ---------------------------------------------------------------------------
cleanup_all
$RTLA osnoise top -q -d 30s -P F:1 >/dev/null 2>&1 &
pid=$!
if ! wait_running "$INSTANCES/osnoise_top" "$pid"; then
	fail "SIGTERM: instance was never created (rtla did not start)"
	stop_pid "$pid"
else
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	if wait_gone "$INSTANCES/osnoise_top"; then
		pass "SIGTERM: instance removed after terminate"
	else
		fail "SIGTERM: instance still present after terminate"
	fi
fi

# ---------------------------------------------------------------------------
# 3. rtla's own -d timeout path
# ---------------------------------------------------------------------------
cleanup_all
$RTLA osnoise top -q -d 2s -P F:1 >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
	fail "self-timeout: rtla failed to start or run (exit=$rc)"
elif wait_gone "$INSTANCES/osnoise_top"; then
	pass "self-timeout: instance removed (exit=$rc)"
else
	fail "self-timeout: instance still present (exit=$rc)"
fi

# ---------------------------------------------------------------------------
# 4. shell action failure (-a / --on-threshold shell,command=...)
# ---------------------------------------------------------------------------
# -S is the stop-total threshold and yields exit 2 when the action fails.
# A shell action only runs if that threshold is crossed; an idle machine may
# never do so.  Do not call the no-threshold case a pass.
cleanup_all
action_rc=0
for n in 1 2 3; do
	$RTLA osnoise top -q -d 5s -P F:1 -r 900000 -S 1 \
		--on-threshold 'shell,command="/bin/false"' >/dev/null 2>&1
	rc=$?
	action_rc=$rc
	[ "$rc" -eq 2 ] && break
done
if [ "$action_rc" -eq 0 ]; then
	# Not a group-level skip: the other cases still ran. Prefix it so the
	# TAP wrapper does not report the whole test as skipped.
	log "note: action-failure: stop-total threshold was not crossed"
elif [ "$action_rc" -ne 2 ]; then
	fail "action-failure: unexpected exit=$action_rc (wanted 2)"
elif wait_gone "$INSTANCES/osnoise_top"; then
	pass "action-failure: instance removed (exit=$action_rc)"
else
	fail "action-failure: instance still present (exit=$action_rc)"
fi

# ---------------------------------------------------------------------------
# 5. repeated interrupted runs must not poison the next run
# ---------------------------------------------------------------------------
cleanup_all
for n in 1 2 3; do
	$RTLA osnoise top -q -d 20s -P F:1 >/dev/null 2>&1 &
	pid=$!
	if ! wait_running "$INSTANCES/osnoise_top" "$pid"; then
		fail "repeated interrupts: run $n never created an instance"
		stop_pid "$pid"
		continue
	fi
	kill -INT "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	if ! wait_gone "$INSTANCES/osnoise_top"; then
		fail "repeated interrupts: run $n left an instance behind"
		cleanup_all
	fi
done

$RTLA osnoise top -q -d 2s -P F:1 >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
	pass "repeated interrupts: following run still succeeds"
else
	fail "repeated interrupts: following run failed (exit=$rc)"
fi
if wait_gone "$INSTANCES/osnoise_top"; then
	pass "repeated interrupts: no leftover instance"
else
	fail "repeated interrupts: leftover instance"
fi

cleanup_all
log ""
log "SUMMARY: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
