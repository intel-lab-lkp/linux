#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Bounded NFQUEUE retention tests; run in a disposable guest with the patch.

The test creates its own network namespace and consumes notifications while
withholding verdicts. --cgroup accepts a writable, delegated cgroup v2 parent
for the optional capacity-reservation test. No public network is used.
"""

import argparse
import contextlib
import errno
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import time


NFQ_SUBSYS = 3 << 8
NFQ_PACKET, NFQ_VERDICT, NFQ_CONFIG, NFQ_BATCH = range(4)
CFG_CMD, CFG_PARAMS, CFG_MAXLEN, CFG_MASK, CFG_FLAGS = range(1, 6)
F_FAIL_OPEN, F_GSO = 1, 4
SKB_GSO = 2
MAX_HELD = 64


def attr(kind, data):
    result = struct.pack("=HH", 4 + len(data), kind) + data
    return result + bytes((-len(result)) % 4)


def u32(kind, value):
    return attr(kind, struct.pack("!I", value))


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class Queue:
    def __init__(self, number=1):
        self.number = number
        self.sock = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 12)
        self.sock.bind((0, 0))
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.seq = 0
        self.held = []

    def close(self):
        self.sock.close()

    def receive(self, timeout):
        if not select.select([self.sock], [], [], timeout)[0]:
            return []
        data, _, flags, _ = self.sock.recvmsg(65536)
        check(not flags & socket.MSG_TRUNC, "truncated netlink message")
        replies = []
        while data:
            check(len(data) >= 16, "short netlink header")
            length, kind, _, seq, _ = struct.unpack_from("=IHHII", data)
            check(16 <= length <= len(data), "invalid netlink length")
            body = data[16:length]
            if kind == 2:  # NLMSG_ERROR, including successful ACKs
                replies.append((seq, struct.unpack_from("=i", body)[0]))
            elif kind == NFQ_SUBSYS | NFQ_PACKET:
                attrs = {}
                offset = 4  # nfgenmsg
                while offset < len(body):
                    size, atype = struct.unpack_from("=HH", body, offset)
                    check(size >= 4 and offset + size <= len(body),
                          "invalid packet attribute")
                    attrs[atype & 0x3fff] = body[offset + 4:offset + size]
                    offset += (size + 3) & ~3
                packet_id = struct.unpack_from("!I", attrs[1])[0]
                skb_info = struct.unpack("!I", attrs.get(14, bytes(4)))[0]
                check(len(self.held) < MAX_HELD, "bounded hold list exhausted")
                self.held.append((packet_id, skb_info))
            data = data[(length + 3) & ~3:]
        return replies

    def request(self, kind, attrs):
        self.seq += 1
        body = struct.pack("!BBH", 0, 0, self.number) + attrs
        header = struct.pack("=IHHII", 16 + len(body), NFQ_SUBSYS | kind,
                             1 | 4, self.seq, 0)  # REQUEST | ACK
        self.sock.sendto(header + body, (0, 0))
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            for seq, error in self.receive(max(0, deadline - time.monotonic())):
                if seq == self.seq:
                    if error:
                        raise OSError(-error, os.strerror(-error))
                    return
        raise TimeoutError("NFQUEUE ACK timed out")

    def configure(self, maximum, *, bind=False, flags=F_GSO, mode=1):
        attrs = u32(CFG_MAXLEN, maximum)
        attrs += attr(CFG_PARAMS, struct.pack("!IB", 0, mode))  # COPY_META by default
        attrs += u32(CFG_MASK, F_GSO | F_FAIL_OPEN) + u32(CFG_FLAGS, flags)
        if bind:
            attrs += attr(CFG_CMD, struct.pack("!BBH", 1, 0, socket.AF_INET))
        self.request(NFQ_CONFIG, attrs)

    def collect(self):
        while select.select([self.sock], [], [], 0.1)[0]:
            replies = self.receive(0)
            check(not replies, "unexpected asynchronous netlink error")

    def release(self, batch=False):
        self.collect()
        ids = [packet_id for packet_id, _ in self.held]
        self.held.clear()
        if batch:
            ids = ids[-1:]
        for packet_id in ids:
            self.request(NFQ_BATCH if batch else NFQ_VERDICT,
                         attr(2, struct.pack("!II", 0, packet_id)))  # DROP

    def stats(self):
        path = Path("/proc/self/net/netfilter/nfnetlink_queue")
        for line in path.read_text().splitlines():
            values = [int(value) for value in line.split()]
            if values[0] == self.number:
                return values
        raise AssertionError("bound queue missing from procfs")


class Traffic:
    def __init__(self):
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("127.0.0.1", 0))
        self.destination = self.rx.getsockname()
        rules = f"""table ip nfq_memory {{
 chain output {{
  type filter hook output priority 0; policy accept;
  udp dport {self.destination[1]} queue num 1
 }}
}}
"""
        subprocess.run(["nft", "-f", "-"], input=rules, text=True, check=True)

    def close(self):
        self.rx.close()

    def send(self, size=16384, segment=0):
        cmsgs = [(socket.IPPROTO_UDP, 103, struct.pack("=H", segment))] if segment else []
        # Retained output skbs can exhaust one sender's socket memory first.
        # A fresh, bounded sender isolates the NFQUEUE retention limit.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.settimeout(1)
            sender.sendmsg([bytes(size)], cmsgs, 0, self.destination)


def retention(traffic, batch):
    with contextlib.closing(Queue()) as queue:
        queue.configure(32, bind=True)
        for _ in range(40):
            traffic.send()
            queue.collect()
        count = len(queue.held)
        check(0 < count < 32, f"expected byte rejection below count limit, held {count}")
        stats = queue.stats()
        check(stats[2] == count and stats[5] > 0, "wrong queue/drop accounting")
        check(stats[6] == 0, "netlink backpressure invalidated the test")
        queue.release(batch=batch)
        check(queue.stats()[2] == 0, "verdict did not drain queue")
        traffic.send()
        queue.collect()
        check(len(queue.held) == 1, "verdict did not restore capacity")


def resize(traffic):
    with contextlib.closing(Queue()) as queue:
        queue.configure(32, bind=True)
        traffic.send()
        queue.collect()
        check(len(queue.held) == 1, "initial packet not queued")
        queue.configure(0)
        check(queue.stats()[2] == 1, "shrink discarded existing entry")
        traffic.send()
        queue.collect()
        check(len(queue.held) == 1, "zero limit admitted a packet")
        queue.release(batch=True)
        queue.configure(32)
        traffic.send()
        queue.collect()
        check(len(queue.held) == 1, "growth did not restore admission")


def oversized(traffic):
    with contextlib.closing(Queue()) as queue:
        queue.configure(1, bind=True)
        traffic.send(60000)
        queue.collect()
        check(not queue.held and queue.stats()[5] == 1,
              "single skb above byte allowance was not rejected")
        queue.configure(1, flags=F_GSO | F_FAIL_OPEN)
        traffic.send(60000)
        queue.collect()
        check(not queue.held, "FAIL_OPEN packet was queued")
        check(select.select([traffic.rx], [], [], 1)[0], "FAIL_OPEN did not deliver")
        check(len(traffic.rx.recv(65536)) == 60000, "incorrect FAIL_OPEN payload")


def ipv4_checksum(header):
    total = sum(struct.unpack("!10H", header))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def payload_accept(traffic):
    with contextlib.closing(Queue()) as queue:
        queue.configure(1, bind=True)
        traffic.send(64)
        queue.collect()
        check(len(queue.held) == 1, "initial small packet was not queued")
        packet_id = queue.held.pop()[0]
        data = b"\xa5" * (60000 - 28)
        address = socket.inet_aton("127.0.0.1")
        header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 60000, 0, 0,
                             64, socket.IPPROTO_UDP, 0, address, address)
        checksum = ipv4_checksum(header)
        header = header[:10] + struct.pack("!H", checksum) + header[12:]
        udp = struct.pack("!HHHH", 12345, traffic.destination[1],
                          len(data) + 8, 0)
        # Delivery proves replacement succeeded; DROP could hide a failed edit.
        queue.request(NFQ_VERDICT,
                      attr(2, struct.pack("!II", 1, packet_id)) +  # ACCEPT
                      attr(10, header + udp + data))  # NFQA_PAYLOAD
        check(select.select([traffic.rx], [], [], 3)[0],
              "expanded ACCEPT packet was not delivered")
        check(traffic.rx.recv(65536) == data,
              "expanded payload differs from supplied data")
        check(queue.stats()[2] == 0, "ACCEPT verdict did not drain queue")
        traffic.send(64)
        queue.collect()
        check(len(queue.held) == 1,
              "successful payload expansion did not restore saved-charge capacity")
        queue.release()
        check(queue.stats()[2] == 0, "subsequent packet did not drain")


def segmentation(traffic):
    for flags, expected in [(F_GSO, 1), (0, 8)]:
        with contextlib.closing(Queue()) as queue:
            queue.configure(64, bind=True, flags=flags)
            try:
                traffic.send(8 * 1200, segment=1200)
            except OSError as error:
                if error.errno in (errno.EINVAL, errno.ENOPROTOOPT, errno.EOPNOTSUPP):
                    return "UDP_SEGMENT is unavailable"
                raise
            queue.collect()
            check(len(queue.held) == expected,
                  f"GSO flags {flags}: expected {expected} entries, got {len(queue.held)}")
            if flags:
                check(queue.held[0][1] & SKB_GSO, "aggregate did not reach NFQUEUE")
            queue.release()
            check(queue.stats()[2] == 0, "segmented entries did not drain")


def reservation(traffic, parent):
    if not parent:
        return "supply --cgroup with a delegated cgroup v2 parent in a test guest"
    parent = Path(parent).resolve()
    check("memory" in (parent / "cgroup.subtree_control").read_text().split(),
          "parent must already delegate the memory controller")
    membership = next(line[3:] for line in Path("/proc/self/cgroup").read_text().splitlines()
                      if line.startswith("0::"))
    original = Path("/sys/fs/cgroup") / membership.lstrip("/")
    group = parent / f"nfq-memory-{os.getpid()}"
    group.mkdir()
    try:
        (group / "memory.max").write_text(str(64 << 20))
        (group / "cgroup.procs").write_text(str(os.getpid()))
        with contextlib.closing(Queue()) as queue:
            try:
                queue.configure(1 << 20, bind=True)
            except OSError as error:
                check(error.errno == errno.ENOMEM, f"unexpected BIND refusal: {error}")
            else:
                raise AssertionError("oversized capacity reservation succeeded")
            # A refused BIND must leave no published queue behind.
            queue.configure(32, bind=True)
            # Later changes must continue charging the original owner.
            (original / "cgroup.procs").write_text(str(os.getpid()))
            try:
                queue.configure(1 << 20, mode=0, flags=0)
            except OSError as error:
                check(error.errno == errno.ENOMEM, f"unexpected growth refusal: {error}")
            else:
                raise AssertionError("growth escaped its original owner")
            traffic.send()
            queue.collect()
            check(len(queue.held) == 1, "failed growth changed the usable queue")
            queue.release()
            queue.configure(0)
        # Individually bounded queues must still share their owner's budget.
        (group / "cgroup.procs").write_text(str(os.getpid()))
        with contextlib.ExitStack() as stack:
            queues = []
            refused = None
            for number in range(10, 26):
                candidate = stack.enter_context(contextlib.closing(Queue(number)))
                try:
                    candidate.configure(4096, bind=True)
                except OSError as error:
                    check(error.errno == errno.ENOMEM,
                          f"unexpected aggregate refusal: {error}")
                    refused = candidate
                    break
                queues.append(candidate)
            check(queues and refused is not None,
                  "expected successful queues followed by aggregate refusal")
            queues[0].configure(0)
            refused.configure(4096, bind=True)
        # Reparent an occupied reservation, then grow and refund it in its parent.
        (group / "cgroup.procs").write_text(str(os.getpid()))
        with contextlib.closing(Queue()) as queue:
            queue.configure(32, bind=True)
            (original / "cgroup.procs").write_text(str(os.getpid()))
            group.rmdir()
            queue.configure(64)
            queue.configure(0)
    finally:
        (original / "cgroup.procs").write_text(str(os.getpid()))
        if group.exists():
            group.rmdir()


def reservation_startup(traffic, parent):
    if not parent:
        return "supply --cgroup with a delegated cgroup v2 parent in a test guest"
    parent = Path(parent).resolve()
    check("memory" in (parent / "cgroup.subtree_control").read_text().split(),
          "parent must already delegate the memory controller")
    membership = next(line[3:] for line in Path("/proc/self/cgroup").read_text().splitlines()
                      if line.startswith("0::"))
    original = Path("/sys/fs/cgroup") / membership.lstrip("/")
    group = parent / f"nfq-startup-{os.getpid()}"
    group.mkdir()
    bind = attr(CFG_CMD, struct.pack("!BBH", 1, 0, socket.AF_INET))
    activate = attr(CFG_PARAMS, struct.pack("!IB", 0, 1))  # COPY_META
    try:
        # Enough for a small queue, but less than the default 1024-slot allowance.
        (group / "memory.max").write_text(str(1 << 20))
        (group / "cgroup.procs").write_text(str(os.getpid()))
        with contextlib.closing(Queue()) as queue:
            queue.request(NFQ_CONFIG, bind)
            check(queue.stats()[3] == 0, "BIND enabled packet admission")
            # Inactive sizing must not reserve capacity either.
            queue.request(NFQ_CONFIG, u32(CFG_MAXLEN, 1 << 20))
            queue.request(NFQ_CONFIG, u32(CFG_MAXLEN, 1))
            (original / "cgroup.procs").write_text(str(os.getpid()))
            try:
                queue.configure(1 << 20, flags=F_GSO | F_FAIL_OPEN)
            except OSError as error:
                check(error.errno == errno.ENOMEM,
                      f"unexpected activation refusal: {error}")
            else:
                raise AssertionError("activation escaped its BIND-time owner")
            check(queue.stats()[3] == 0, "failed activation changed copy mode")
            # A mode-only retry also verifies that failed activation kept MAXLEN.
            queue.request(NFQ_CONFIG, activate)
            traffic.send(64)
            queue.collect()
            check(len(queue.held) == 1, "small queue did not become usable")
            queue.request(NFQ_CONFIG, attr(CFG_PARAMS, struct.pack("!IB", 0, 0)))
            stats = queue.stats()
            check(stats[3] == 0 and stats[2] == 1,
                  "COPY_NONE discarded a retained packet")
            try:
                queue.request(NFQ_CONFIG, u32(CFG_MAXLEN, 1 << 20))
            except OSError as error:
                check(error.errno == errno.ENOMEM,
                      f"unexpected COPY_NONE growth refusal: {error}")
            else:
                raise AssertionError("COPY_NONE disabled established accounting")
            stats = queue.stats()
            check(stats[3] == 0 and stats[2] == 1 and len(queue.held) == 1,
                  "failed COPY_NONE growth changed mode or retained packet")
            queue.request(NFQ_CONFIG, activate)
            queue.release()
            traffic.send(60000)
            queue.collect()
            check(not queue.held and queue.stats()[5] > 0,
                  "failed activation changed the small queue limit")
            check(not select.select([traffic.rx], [], [], 0.1)[0],
                  "failed activation enabled FAIL_OPEN")

        # BIND, final size, and mode may also arrive in one configuration message.
        (group / "cgroup.procs").write_text(str(os.getpid()))
        with contextlib.closing(Queue()) as queue:
            queue.configure(1, bind=True)
            (original / "cgroup.procs").write_text(str(os.getpid()))
            traffic.send(64)
            queue.collect()
            check(len(queue.held) == 1, "bundled small-queue activation failed")
            queue.release()

        # Closing before activation must release the captured owner reference.
        (group / "cgroup.procs").write_text(str(os.getpid()))
        with contextlib.closing(Queue()) as queue:
            queue.request(NFQ_CONFIG, bind)
            (original / "cgroup.procs").write_text(str(os.getpid()))
    finally:
        (original / "cgroup.procs").write_text(str(os.getpid()))
        group.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cgroup")
    parser.add_argument("--inside-netns", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not all(shutil.which(tool) for tool in ("nft", "ip", "unshare")):
        print("1..0 # SKIP nft, ip and unshare are required")
        return 4
    if not args.inside_netns:
        probe = subprocess.run(["unshare", "-n", "true"], capture_output=True)
        if probe.returncode:
            print("1..0 # SKIP cannot create a network namespace")
            return 4
        return subprocess.call(["unshare", "-n", sys.executable,
                                str(Path(__file__).resolve()), "--inside-netns",
                                *(["--cgroup", args.cgroup] if args.cgroup else [])])
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    with contextlib.closing(Traffic()) as traffic:
        tests = [
            ("byte admission and individual release", lambda: retention(traffic, False)),
            ("byte admission and batch release", lambda: retention(traffic, True)),
            ("occupied shrink and growth", lambda: resize(traffic)),
            ("oversized skb and FAIL_OPEN", lambda: oversized(traffic)),
            ("payload replacement and saved-charge release", lambda: payload_accept(traffic)),
            ("GSO and software segmentation", lambda: segmentation(traffic)),
            ("reservation rollback and ownership", lambda: reservation(traffic, args.cgroup)),
            ("reservation startup and activation", lambda: reservation_startup(traffic, args.cgroup)),
        ]
        print(f"TAP version 13\n1..{len(tests)}", flush=True)
        failed = False
        for number, (name, run) in enumerate(tests, 1):
            try:
                skip = run()
                suffix = f" # SKIP {skip}" if skip else ""
                print(f"ok {number} - {name}{suffix}", flush=True)
            except (AssertionError, OSError, TimeoutError) as error:
                print(f"not ok {number} - {name}\n# {error}", flush=True)
                failed = True
        return int(failed)


if __name__ == "__main__":
    sys.exit(main())
