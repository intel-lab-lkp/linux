#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# End-to-end killswitch selftest.  Drives the test_killswitch module
# through an engage/disengage cycle and confirms each transition
# behaves as expected.  Also runs the AF_ALG mitigation proof.
#
# Requirements (see Documentation/admin-guide/killswitch.rst):
#   - CONFIG_KILLSWITCH=y
#   - CONFIG_TEST_KILLSWITCH=m
#   - run as root (CAP_SYS_ADMIN)
#
# Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
#

set -u

KS=/sys/kernel/security/killswitch
TRIG=/sys/kernel/debug/test_killswitch/fire

NOMOD=0
SKIP_RC=4
N=0
FAIL=0

ksft_pass() { N=$((N+1));    echo "ok $N - $*"; }
ksft_fail() { N=$((N+1)); FAIL=$((FAIL+1)); echo "not ok $N - $*"; }
ksft_skip() { echo "ok 1 - SKIP $*"; echo "1..1"; exit $SKIP_RC; }

[[ $EUID -eq 0 ]] || ksft_skip "must be root"
[[ -d $KS    ]] || ksft_skip "$KS not present (CONFIG_KILLSWITCH disabled?)"

if ! modprobe test_killswitch 2>/dev/null; then
	NOMOD=1
fi
[[ -e $TRIG ]] || ksft_skip "$TRIG missing (test_killswitch.ko not installed?)"

cleanup() {
	echo "disengage_all" > $KS/control 2>/dev/null || true
	[[ $NOMOD -eq 0 ]] && rmmod test_killswitch 2>/dev/null || true
}
trap cleanup EXIT

# --- pre-engage: bad path runs, write fails with EBADMSG ---
if echo 0xC0FFEE > $TRIG 2>/dev/null; then
	ksft_fail "pre-engage: write should have failed (-EBADMSG)"
else
	[[ $? -ne 0 ]] && ksft_pass "pre-engage: bad path returns error" \
	             || ksft_fail "pre-engage: unexpected outcome"
fi

# --- engage ---
echo "engage ks_test_vuln 0" > $KS/control
grep -q "^ks_test_vuln" $KS/engaged \
	&& ksft_pass "engage: ks_test_vuln in engaged list" \
	|| ksft_fail "engage: missing from engaged list"

[[ $(cat $KS/taint) == 1 ]] \
	&& ksft_pass "engage: taint set" \
	|| ksft_fail "engage: taint not set"

[[ -d $KS/fn/ks_test_vuln ]] \
	&& ksft_pass "engage: per-fn dir created" \
	|| ksft_fail "engage: per-fn dir missing"

# --- post-engage: BUG suppressed; write returns successfully ---
if echo 0xC0FFEE > $TRIG 2>/dev/null; then
	ksft_pass "post-engage: BUG suppressed, write succeeded"
else
	ksft_fail "post-engage: write should succeed"
fi

[[ $(cat $KS/fn/ks_test_vuln/hits) -ge 1 ]] \
	&& ksft_pass "post-engage: hits counter incremented" \
	|| ksft_fail "post-engage: hits counter did not move"

# --- retval rewrite is a plain write (no validation) ---
echo 7 > $KS/fn/ks_test_vuln/retval
[[ $(cat $KS/fn/ks_test_vuln/retval) == 7 ]] \
	&& ksft_pass "retval rewrite round-trips" \
	|| ksft_fail "retval rewrite failed"

# --- engage on a kprobe-rejected function fails ---
# warn_thunk_thunk is in /sys/kernel/debug/kprobes/blacklist;
# register_kprobe() refuses it.
KP_REJECT=warn_thunk_thunk
if echo "engage $KP_REJECT 0" > $KS/control 2>/dev/null; then
	ksft_fail "register_kprobe should have rejected $KP_REJECT"
	echo "disengage $KP_REJECT" > $KS/control
else
	ksft_pass "register_kprobe refuses blacklisted target"
fi

# --- disengage ---
echo "disengage ks_test_vuln" > $KS/control
[[ -z "$(cat $KS/engaged)" ]] \
	&& ksft_pass "disengage: engaged list empty" \
	|| ksft_fail "disengage: engaged list not empty"

[[ ! -d $KS/fn/ks_test_vuln ]] \
	&& ksft_pass "disengage: per-fn dir removed" \
	|| ksft_fail "disengage: per-fn dir still present"

[[ $(cat $KS/taint) == 1 ]] \
	&& ksft_pass "disengage: taint persists" \
	|| ksft_fail "disengage: taint should persist"

# --- post-disengage: bad path active again ---
if echo 0xC0FFEE > $TRIG 2>/dev/null; then
	ksft_fail "post-disengage: write should fail again"
else
	ksft_pass "post-disengage: bad path active again"
fi

# ---- CVE-2026-31431 mitigation proof (AF_ALG aead via af_alg_sendmsg) ----
# Skip the whole block if AF_ALG / AEAD machinery isn't compiled in.
if [[ -x $(dirname "$0")/cve_31431_test ]]; then
	CVE=$(dirname "$0")/cve_31431_test
	$CVE >/dev/null 2>&1 && PRE=$? || PRE=$?
	if [[ $PRE -eq 0 ]]; then
		ksft_pass "cve-31431: pre-engage AEAD round-trip OK"

		echo "engage af_alg_sendmsg -1" > $KS/control
		$CVE >/dev/null 2>&1 && POST=$? || POST=$?
		if [[ $POST -eq 1 ]]; then
			ksft_pass "cve-31431: post-engage AEAD refused (mitigated)"
		else
			ksft_fail "cve-31431: post-engage exit=$POST (expected 1)"
		fi

		HITS=$(cat $KS/fn/af_alg_sendmsg/hits 2>/dev/null || echo 0)
		[[ $HITS -ge 1 ]] && ksft_pass "cve-31431: hits=$HITS recorded" \
			|| ksft_fail "cve-31431: hits not recorded"

		echo "disengage af_alg_sendmsg" > $KS/control
		$CVE >/dev/null 2>&1 && POST2=$? || POST2=$?
		[[ $POST2 -eq 0 ]] && ksft_pass "cve-31431: post-disengage restored" \
			|| ksft_fail "cve-31431: post-disengage exit=$POST2"
	elif [[ $PRE -eq 2 ]]; then
		echo "# SKIP cve-31431 (AF_ALG/AEAD not available)"
	else
		ksft_fail "cve-31431: pre-engage exit=$PRE"
	fi
fi

# ---- CVE-2026-43284 mitigation proof (IPsec ESP via esp_input) ----
# Engaging esp_input causes inbound ESP packets to be dropped before
# decapsulation, neutering any bug downstream of the ESP receive path.
# Two netns + veth so traffic actually traverses xfrm (single-netns
# 127.0.0.0/8 traffic short-circuits before xfrm policy lookup).
NS0=ks-esp-0
NS1=ks-esp-1
esp_setup_ok=0
esp_cleanup() {
	[[ $esp_setup_ok -eq 1 ]] || return 0
	ip netns del $NS0 2>/dev/null
	ip netns del $NS1 2>/dev/null
}
trap 'cleanup; esp_cleanup' EXIT

# UDP probe in python3 (always present on Debian/Fedora minimal installs).
esp_round_trip() {
	# $1: source netns, $2: dest netns, $3: dest ip, $4: port
	local tmp rpid rc
	tmp=$(mktemp)
	ip netns exec "$2" python3 -c '
import socket
r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
r.bind(("0.0.0.0", '"$4"'))
r.settimeout(2.0)
try:
    d,_ = r.recvfrom(64)
    print(d.decode(errors="replace"))
except socket.timeout:
    print("timeout")
' > "$tmp" 2>&1 &
	rpid=$!
	sleep 0.3
	ip netns exec "$1" python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"ks-esp-probe", ("'"$3"'", '"$4"'))
' 2>/dev/null
	wait $rpid 2>/dev/null
	rc=1
	grep -q "ks-esp-probe" "$tmp" && rc=0
	rm -f "$tmp"
	return $rc
}

if command -v ip >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
	KEY=0x0123456789abcdef0123456789abcdef01234567

	if ip netns add $NS0 2>/dev/null && \
	   ip netns add $NS1 2>/dev/null && \
	   ip link add veth0 type veth peer name veth1 2>/dev/null && \
	   ip link set veth0 netns $NS0 2>/dev/null && \
	   ip link set veth1 netns $NS1 2>/dev/null && \
	   ip -n $NS0 addr add 10.99.0.1/24 dev veth0 2>/dev/null && \
	   ip -n $NS1 addr add 10.99.0.2/24 dev veth1 2>/dev/null && \
	   ip -n $NS0 link set veth0 up 2>/dev/null && \
	   ip -n $NS1 link set veth1 up 2>/dev/null && \
	   ip -n $NS0 link set lo up 2>/dev/null && \
	   ip -n $NS1 link set lo up 2>/dev/null && \
	   ip -n $NS0 xfrm state add src 10.99.0.1 dst 10.99.0.2 proto esp \
		spi 0x1000 mode transport reqid 0x100 \
		aead 'rfc4106(gcm(aes))' $KEY 128 2>/dev/null && \
	   ip -n $NS0 xfrm state add src 10.99.0.2 dst 10.99.0.1 proto esp \
		spi 0x1001 mode transport reqid 0x100 \
		aead 'rfc4106(gcm(aes))' $KEY 128 2>/dev/null && \
	   ip -n $NS1 xfrm state add src 10.99.0.1 dst 10.99.0.2 proto esp \
		spi 0x1000 mode transport reqid 0x100 \
		aead 'rfc4106(gcm(aes))' $KEY 128 2>/dev/null && \
	   ip -n $NS1 xfrm state add src 10.99.0.2 dst 10.99.0.1 proto esp \
		spi 0x1001 mode transport reqid 0x100 \
		aead 'rfc4106(gcm(aes))' $KEY 128 2>/dev/null && \
	   ip -n $NS0 xfrm policy add src 10.99.0.1 dst 10.99.0.2 \
		dir out tmpl src 10.99.0.1 dst 10.99.0.2 proto esp \
		reqid 0x100 mode transport 2>/dev/null && \
	   ip -n $NS1 xfrm policy add src 10.99.0.1 dst 10.99.0.2 \
		dir in tmpl src 10.99.0.1 dst 10.99.0.2 proto esp \
		reqid 0x100 mode transport 2>/dev/null; then
		esp_setup_ok=1
	fi

	if [[ $esp_setup_ok -eq 1 ]] \
	   && esp_round_trip $NS0 $NS1 10.99.0.2 53435; then
		ksft_pass "cve-43284: pre-engage ESP round-trip OK"

		echo "engage esp_input -22" > $KS/control
		if esp_round_trip $NS0 $NS1 10.99.0.2 53435; then
			ksft_fail "cve-43284: post-engage ESP should have been dropped"
		else
			ksft_pass "cve-43284: post-engage ESP refused (mitigated)"
		fi

		ESP_HITS=$(cat $KS/fn/esp_input/hits 2>/dev/null || echo 0)
		[[ $ESP_HITS -ge 1 ]] \
			&& ksft_pass "cve-43284: hits=$ESP_HITS recorded" \
			|| ksft_fail "cve-43284: hits not recorded"

		echo "disengage esp_input" > $KS/control
		if esp_round_trip $NS0 $NS1 10.99.0.2 53435; then
			ksft_pass "cve-43284: post-disengage restored"
		else
			ksft_fail "cve-43284: post-disengage ESP still dropped"
		fi
	else
		echo "# SKIP cve-43284 (netns/veth/XFRM/ESP setup failed)"
	fi
fi

echo "1..$N"
exit $((FAIL > 0))
