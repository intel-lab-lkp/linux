#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Verify that per-egress mutations of shared skb data are private:
#
# F2 (path/LAN ID): on an affected kernel the second slave's LAN-ID write
# lands in the first slave's still-queued clone; with a netem delay on
# slave A, injected frames leave A carrying B's LAN ID.
#
# F1 (RedBox source MAC): on an affected kernel an HSR-tagged multicast
# frame received on a RedBox slave is cloned for master and interlink,
# and the interlink's RedBox-MAC rewrite lands in the master clone's
# buffer, so the local stack receives the RedBox MAC instead of the
# originating node's MAC.

ipv6=false

source ./hsr_common.sh

DUR=5

require()
{
	command -v "$1" >/dev/null 2>&1 && return 0
	echo "SKIP: $1 not available"
	exit $ksft_skip
}

require ip
require tc
require python3

trap cleanup_all_ns EXIT

# ------------------------------------------------------- F2: LAN-ID isolation
# PRP DANP (proto 1), AF_PACKET pre-tagged injection, netem on slave A.
run_f2()
{
	setup_ns ns 2>/dev/null || return $ksft_skip
	nsx() { ip netns exec "$ns" "$@"; }

	# Probe sch_netem inside the disposable namespace only.
	if ! nsx tc qdisc add dev lo root netem delay 1ms 2>/dev/null; then
		echo "SKIP: sch_netem not available"
		return $ksft_skip
	fi
	nsx tc qdisc del dev lo root 2>/dev/null

	# Capability probes end here; setup or runtime failure below is FAIL.
	nsx ip link add vA type veth peer name vAp || { echo "FAIL: veth A"; return 1; }
	nsx ip link add vB type veth peer name vBp || { echo "FAIL: veth B"; return 1; }
	for i in vA vB vAp vBp; do
		nsx ip link set "$i" up || { echo "FAIL: $i up"; return 1; }
	done
	nsx ip link add name prp0 type hsr slave1 vA slave2 vB supervision 45 proto 1 2>/dev/null
	if [ $? -ne 0 ]; then
		echo "SKIP: HSR/PRP not supported by this kernel"
		return $ksft_skip
	fi
	nsx ip link set prp0 up || { echo "FAIL: prp0 up"; return 1; }
	nsx tc qdisc add dev vA root netem delay 200ms || { echo "FAIL: netem"; return 1; }

	nsx python3 /dev/stdin "$DUR" <<'PYF2'
import socket, struct, select, sys, time

dur = int(sys.argv[1])
def lanid(pkt):
    if len(pkt) < 20 or pkt[-2:] != b"\x88\xfb":
        return None
    return (pkt[-4] >> 4) & 0xF

SRC = bytes.fromhex(open("/sys/class/net/prp0/address").read().replace(":", ""))
DST = bytes.fromhex("02aabbccdd01")
PAY = bytes(range(46))
rct0 = struct.pack(">H", 0) + struct.pack(">H", 52 & 0x0FFF) + b"\x88\xfb"
frame = DST + SRC + b"\x08\x00" + PAY + rct0

tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW); tx.bind(("prp0", 0))
sA = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
        socket.ntohs(0x0003))
sA.bind(("vAp", 0))
sB = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
        socket.ntohs(0x0003))
sB.bind(("vBp", 0))
sA.setblocking(False); sB.setblocking(False)
for _ in range(200):
    tx.send(frame); time.sleep(0.001)

a, b = [], []
end = time.time() + dur
while time.time() < end:
    r, _, _ = select.select([sA, sB], [], [], 0.3)
    for s in r:
        pkt = s.recv(65535)
        if (pkt[:6] != DST or pkt[6:12] != SRC or pkt[12:14] != b"\x08\x00"
                or pkt[14:14 + len(PAY)] != PAY):
            continue
        lid = lanid(pkt)
        if lid is not None:
            (a if s is sA else b).append(lid)

print("A-side count=%d lan ids=%s" % (len(a), sorted(set(a))))
print("B-side count=%d lan ids=%s" % (len(b), sorted(set(b))))
if len(a) < 150 or len(b) < 150:
    print("FAIL: too few injected frames captured (A=%d B=%d, sent 200)" % (len(a), len(b)))
    sys.exit(1)
bad_a = [x for x in a if (x & 1) != 0]
bad_b = [x for x in b if (x & 1) != 1]
if bad_a or bad_b:
    print("FAIL: shared-mutation corruption - A: %d/%d wrong-lan, B: %d/%d wrong-lan"
          % (len(bad_a), len(a), len(bad_b), len(b)))
    sys.exit(1)
print("PASS: per-egress LAN IDs isolated (A all bit0=0, B all bit0=1)")
sys.exit(0)
PYF2
}

# --------------------------------------------- F1: RedBox source-MAC privacy
# HSR RedBox (proto 0), tagged multicast from a slave: master must keep
# the node MAC, interlink must carry the RedBox MAC.
run_f1()
{
	setup_ns ns 2>/dev/null || return $ksft_skip
	nsx() { ip netns exec "$ns" "$@"; }

	nsx ip link add vA type veth peer name vAp || { echo "FAIL: veth A"; return 1; }
	nsx ip link add vB type veth peer name vBp || { echo "FAIL: veth B"; return 1; }
	nsx ip link add vI type veth peer name vIp || { echo "FAIL: veth I"; return 1; }
	for i in vA vB vI vAp vBp vIp; do
		nsx ip link set "$i" up || { echo "FAIL: $i up"; return 1; }
	done
	nsx ip link add name hsr0 type hsr slave1 vA slave2 vB interlink vI \
		supervision 45 proto 0 2>/dev/null
	if [ $? -ne 0 ]; then
		echo "SKIP: HSR RedBox not supported by this kernel"
		return $ksft_skip
	fi
	nsx ip link set hsr0 up || { echo "FAIL: hsr0 up"; return 1; }

	nsx python3 /dev/stdin <<'PYF1'
import socket, select, sys, time

NODE  = bytes.fromhex("021122334455")
MCAST = bytes.fromhex("01005e000001")
RB    = bytes.fromhex(open("/sys/class/net/vI/address").read().replace(":", ""))
PAY   = bytes(range(46))

def frame(seq):
    tag = ((1 << 12) | len(PAY)).to_bytes(2, "big") + seq.to_bytes(2, "big") + b"\x08\x00"
    return MCAST + NODE + b"\x89\x2f" + tag + PAY

tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW); tx.bind(("vAp", 0))
sm = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
        socket.ntohs(0x0003))
sm.bind(("hsr0", 0))
si = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
        socket.ntohs(0x0003))
si.bind(("vIp", 0))
sm.setblocking(False); si.setblocking(False)

for i in range(3):
    tx.send(frame(i + 1)); time.sleep(0.05)

m_src = i_src = None
end = time.time() + 4
while time.time() < end and (m_src is None or i_src is None):
    r, _, _ = select.select([sm, si], [], [], 0.3)
    for s in r:
        pkt = s.recv(65535)
        # exact flow: dst, post-strip EtherType, exact payload, min length;
        # h_source is the asserted value and must NOT be filtered on
        if (len(pkt) < 60 or pkt[:6] != MCAST or pkt[12:14] != b"\x08\x00"
                or pkt[14:14 + len(PAY)] != PAY):
            continue
        if s is sm and m_src is None:
            m_src = pkt[6:12]
        elif s is si and i_src is None:
            i_src = pkt[6:12]

print("master h_source    =", m_src.hex() if m_src else None)
print("node MAC           =", NODE.hex())
print("interlink h_source =", i_src.hex() if i_src else None)
print("redbox MAC         =", RB.hex())
if i_src != RB:
    print("FAIL: interlink did not carry the RedBox MAC")
    sys.exit(1)
if m_src != NODE:
    print("FAIL: master received %s instead of the node MAC "
          "(shared-mutation corruption)" % (m_src.hex() if m_src else "nothing"))
    sys.exit(1)
print("PASS: master kept node MAC, interlink kept RedBox MAC")
sys.exit(0)
PYF1
}

rc=0

run_f2
ret=$?
[ "$ret" -eq "$ksft_skip" ] && exit "$ksft_skip"
[ "$ret" -eq 0 ] || rc=1

run_f1
ret=$?
[ "$ret" -eq "$ksft_skip" ] && exit "$ksft_skip"
[ "$ret" -eq 0 ] || rc=1

if [ $rc -eq 0 ]; then
	echo "hsr_shared_mutation: per-egress mutation isolation (F1+F2) [ OK ]"
else
	echo "hsr_shared_mutation: per-egress mutation isolation [ FAIL ] rc=$rc" 1>&2
fi
exit $rc
