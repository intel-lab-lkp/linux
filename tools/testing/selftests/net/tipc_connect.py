#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Exercise pending TIPC connects across bearer loss, before accept()."""

import errno
import os
import select
import shutil
import socket
import threading
import time
from contextlib import ExitStack, contextmanager

from lib.py import (KsftNamedVariant, KsftSkipEx, NetNS, NetNSEnter,
                    cmd, ip, ksft_eq, ksft_exit, ksft_run, ksft_true,
                    ksft_variants)


SOCKET_TYPES = [KsftNamedVariant("stream", socket.SOCK_STREAM),
                KsftNamedVariant("seqpacket", socket.SOCK_SEQPACKET)]
CONNECT_TIMEOUT = 10000
RETURN_TIMEOUT = 2


def tipc(ns, args):
    return cmd("tipc " + args, ns=ns).stdout


def unicast_links_up(output):
    """Return distinct UP links, excluding the always-UP broadcast link."""
    links = set()
    for line in output.splitlines():
        name, separator, state = line.strip().rpartition(": ")
        if (separator and state.lower() == "up" and
                not name.startswith("broadcast-link")):
            links.add(name)
    return links


def wait_for_links(client_ns, server_ns, links):
    deadline = time.monotonic() + 10
    while True:
        # These isolated namespaces contain only the two test peers. Count
        # their unicast links, not the namespace's always-UP broadcast link.
        outputs = [tipc(ns, "link list") for ns in (client_ns, server_ns)]
        if all(len(unicast_links_up(output)) == links for output in outputs):
            return
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"Expected {links} UP unicast links at each peer; "
                f"client link list: {outputs[0]!r}; "
                f"server link list: {outputs[1]!r}")
        time.sleep(0.05)


@contextmanager
def topology(sock_type, links=1):
    if not hasattr(socket, "AF_TIPC"):
        raise KsftSkipEx("Python lacks AF_TIPC support")
    if os.geteuid() != 0:
        raise KsftSkipEx("root privileges required for network namespaces")
    for tool in ("ip", "tipc"):
        if not shutil.which(tool):
            raise KsftSkipEx(f"{tool} is required")
    try:
        with socket.socket(socket.AF_TIPC, sock_type):
            pass
    except OSError as error:
        if error.errno in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT):
            raise KsftSkipEx("TIPC support is unavailable; load tipc") from error
        raise

    with ExitStack() as stack:
        client_ns = stack.enter_context(NetNS())
        server_ns = stack.enter_context(NetNS())
        for ns, address in ((client_ns, "1.1.1"), (server_ns, "1.1.2")):
            tipc(ns, "node set netid 4711")
            tipc(ns, f"node set address {address}")
        for index in range(links):
            ip(f"link add c{index} type veth peer name s{index} "
               f"netns {server_ns}", ns=client_ns)
            for ns, dev in ((client_ns, f"c{index}"),
                            (server_ns, f"s{index}")):
                ip(f"link set {dev} up", ns=ns)
                tipc(ns, f"bearer enable media eth device {dev}")

        wait_for_links(client_ns, server_ns, links)

        with NetNSEnter(server_ns):
            listener = stack.enter_context(socket.socket(socket.AF_TIPC,
                                                        sock_type))
            listener.listen(8)
        with NetNSEnter(client_ns):
            client = stack.enter_context(socket.socket(socket.AF_TIPC,
                                                      sock_type))
            client.setsockopt(socket.SOL_TIPC, socket.TIPC_CONN_TIMEOUT,
                              CONNECT_TIMEOUT)
        try:
            yield client_ns, client, listener
        except Exception:
            # Collect diagnostics before ExitStack destroys the topology.
            for label, ns in (("client", client_ns), ("server", server_ns)):
                for args in ("link list", "node list"):
                    try:
                        output = tipc(ns, args)
                        print(f"# {label} {args}: {output!r}")
                    except Exception as error:
                        print(f"# {label} {args} failed: {error}")
            raise


@contextmanager
def pending_connect(client, listener, address=None):
    result = []
    if address is None:
        address = listener.getsockname()

    def connect():
        result.append(client.connect_ex(address))

    worker = threading.Thread(target=connect)
    worker.start()
    try:
        # Readability proves SYN is queued on the listener, without accepting it.
        if not select.select([listener], [], [], 5)[0]:
            raise TimeoutError(f"SYN did not reach {address!r}: {result}")
        ksft_eq(result, [], "connect must wait for accept")
        yield worker, result
    finally:
        # Bound cleanup even on the unfixed kernel or a failed assertion.
        if worker.is_alive():
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        worker.join(CONNECT_TIMEOUT / 1000 + 1)


def drop_link(ns, index):
    tipc(ns, f"bearer disable media eth device c{index}")


def check_node_loss(client):
    # TIPC_DISCONNECTING reports POLLHUP, not POLLOUT. Unlike select()'s
    # write set, poll() reports hangup even when only POLLOUT is requested.
    poller = select.poll()
    poller.register(client, select.POLLOUT)
    events = poller.poll(int(RETURN_TIMEOUT * 1000))
    ksft_true(events, "node loss must notify pending connect within 2s")
    if events:
        ksft_eq(events[0][0], client.fileno())
        ksft_true(events[0][1] & select.POLLHUP,
                  f"node loss must report hangup, got {events!r}")
    ksft_eq(client.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR),
            errno.EHOSTUNREACH)


@ksft_variants(SOCKET_TYPES)
def normal_accept(sock_type):
    with topology(sock_type) as (_, client, listener):
        with pending_connect(client, listener) as (worker, result):
            with listener.accept()[0] as accepted:
                worker.join(RETURN_TIMEOUT)
                ksft_eq(result, [0])
                if result == [0]:
                    accepted.settimeout(RETURN_TIMEOUT)
                    client.sendall(b"connected")
                    ksft_eq(accepted.recv(32), b"connected")


@ksft_variants(SOCKET_TYPES)
def last_link_down(sock_type):
    with topology(sock_type) as (ns, client, listener):
        with pending_connect(client, listener) as (worker, result):
            drop_link(ns, 0)
            worker.join(RETURN_TIMEOUT)
            ksft_eq(result, [errno.EHOSTUNREACH],
                    "node loss must abort connect before its 10s timeout")


@ksft_variants(SOCKET_TYPES)
def surviving_link(sock_type):
    with topology(sock_type, links=2) as (ns, client, listener):
        with pending_connect(client, listener) as (worker, result):
            drop_link(ns, 0)
            worker.join(0.2)
            ksft_eq(result, [], "one remaining link must preserve connect")
            with listener.accept()[0]:
                worker.join(RETURN_TIMEOUT)
                ksft_eq(result, [0])


@ksft_variants(SOCKET_TYPES)
def both_links_down(sock_type):
    with topology(sock_type, links=2) as (ns, client, listener):
        with pending_connect(client, listener) as (worker, result):
            drop_link(ns, 0)
            worker.join(0.2)
            ksft_eq(result, [])
            drop_link(ns, 1)
            worker.join(RETURN_TIMEOUT)
            ksft_eq(result, [errno.EHOSTUNREACH])


@ksft_variants(SOCKET_TYPES)
def rejected_connect(sock_type):
    with topology(sock_type) as (ns, client, listener):
        with pending_connect(client, listener) as (worker, result):
            listener.close()
            worker.join(RETURN_TIMEOUT)
            ksft_eq(result, [errno.ECONNREFUSED])
            drop_link(ns, 0)
            ksft_eq(client.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR), 0,
                    "node loss must not change a completed rejection")


@ksft_variants(SOCKET_TYPES)
def connect_timeout_then_node_loss(sock_type):
    with topology(sock_type) as (ns, client, listener):
        client.setsockopt(socket.SOL_TIPC, socket.TIPC_CONN_TIMEOUT, 500)
        with pending_connect(client, listener) as (worker, result):
            worker.join(RETURN_TIMEOUT)
            ksft_eq(result, [errno.ETIMEDOUT])
            # A wait timeout leaves the handshake pending, just as EINPROGRESS.
            drop_link(ns, 0)
            check_node_loss(client)


@ksft_variants(SOCKET_TYPES)
def nonblocking_connect(sock_type):
    with topology(sock_type) as (ns, client, listener):
        client.setblocking(False)
        ksft_eq(client.connect_ex(listener.getsockname()), errno.EINPROGRESS)
        if not select.select([listener], [], [], 5)[0]:
            raise TimeoutError("SYN did not reach listener")
        drop_link(ns, 0)
        check_node_loss(client)


def publish_service(ns, listener):
    service = 18888
    listener.bind((socket.TIPC_ADDR_NAMESEQ, service, 1, 1,
                   socket.TIPC_CLUSTER_SCOPE))
    deadline = time.monotonic() + 10
    while str(service) not in tipc(ns, "nametable show").split():
        if time.monotonic() >= deadline:
            raise TimeoutError("TIPC service publication did not arrive")
        time.sleep(0.05)
    return socket.TIPC_ADDR_NAME, service, 1, 0


@ksft_variants(SOCKET_TYPES)
def named_accept(sock_type):
    with topology(sock_type) as (ns, client, listener):
        address = publish_service(ns, listener)
        with pending_connect(client, listener, address) as (worker, result):
            with listener.accept()[0]:
                worker.join(RETURN_TIMEOUT)
                ksft_eq(result, [0])


@ksft_variants(SOCKET_TYPES)
def named_node_loss(sock_type):
    with topology(sock_type) as (ns, client, listener):
        address = publish_service(ns, listener)
        with pending_connect(client, listener, address) as (worker, result):
            drop_link(ns, 0)
            worker.join(RETURN_TIMEOUT)
            ksft_eq(result, [errno.EHOSTUNREACH])


if __name__ == "__main__":
    ksft_run(cases=[normal_accept, last_link_down, surviving_link,
                    both_links_down, rejected_connect,
                    connect_timeout_then_node_loss, nonblocking_connect,
                    named_accept, named_node_loss])
    ksft_exit()
