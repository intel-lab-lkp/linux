#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import ctypes
import errno
import hashlib
import os
import select
import socket
import sys

# Allow utils module to be imported from different directory
this_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(this_dir, "../"))
from lib.py.utils import ip

libc = ctypes.cdll.LoadLibrary('libc.so.6')
setns = libc.setns

# Helper function for creating a socket inside a network namespace.
# We need this because otherwise RDS will detect that the two TCP
# sockets are on the same interface and use the loop transport instead
# of the TCP transport.
def netns_socket(netns, *args):
    """Create a socket inside a network namespace."""
    u0, u1 = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)

    child = os.fork()
    if child == 0:
        # change network namespace
        with open(f'/var/run/netns/{netns}', encoding='utf-8') as f:
            try:
                setns(f.fileno(), 0)
            except IOError as ioe:
                print(ioe.errno)
                print(ioe)

        # create socket in target namespace
        s = socket.socket(*args)

        # send resulting socket to parent
        socket.send_fds(u0, [], [s.fileno()])

        sys.exit(0)

    # receive socket from child
    _, s, _, _ = socket.recv_fds(u1, 0, 1)
    os.waitpid(child, 0)
    u0.close()
    u1.close()
    return socket.fromfd(s[0], *args)

def run_test(env):
    """Run basic RDS selftest.

    env is a dictionary provided by test.py and is expected to contain:
      - 'addrs':   list of (ip, port) tuples matching the sockets
      - 'netns':   list of network namespace names (for sysctl exercises)
    """
    addrs = env['addrs']        # [('10.0.0.1', 10000), ('10.0.0.2', 20000)]
    netns_list = env['netns']   # ['net0', 'net1']

    sockets = [
        netns_socket(netns_list[0], socket.AF_RDS, socket.SOCK_SEQPACKET),
        netns_socket(netns_list[1], socket.AF_RDS, socket.SOCK_SEQPACKET),
    ]

    for s, addr in zip(sockets, addrs):
        s.bind(addr)
        s.setblocking(0)

    fileno_to_socket = {
        s.fileno(): s for s in sockets
    }

    addr_to_socket = dict(zip(addrs, sockets))

    socket_to_addr = {
        s: addr for addr, s in zip(addrs, sockets)
    }

    send_hashes = {}
    recv_hashes = {}

    ep = select.epoll()

    for s in sockets:
        ep.register(s, select.EPOLLRDNORM)

    n = 50000
    nr_send = 0
    nr_recv = 0

    while nr_send < n:
        # Send as much as we can without blocking
        print("sending...", nr_send, nr_recv)
        while nr_send < n:
            send_data = hashlib.sha256(
                f'packet {nr_send}'.encode('utf-8')).hexdigest().encode('utf-8')

            # pseudo-random send/receive pattern
            sender = sockets[nr_send % 2]
            receiver = sockets[1 - (nr_send % 3) % 2]

            try:
                sender.sendto(send_data, socket_to_addr[receiver])
                send_hashes.setdefault((sender.fileno(), receiver.fileno()),
                        hashlib.sha256()).update(f'<{send_data}>'.encode('utf-8'))
                nr_send = nr_send + 1
            except BlockingIOError:
                break
            except OSError as e:
                if e.errno in [errno.ENOBUFS, errno.ECONNRESET, errno.EPIPE]:
                    break
                raise

        # Receive as much as we can without blocking
        print("receiving...", nr_send, nr_recv)
        while nr_recv < nr_send:
            for fileno, eventmask in ep.poll():
                receiver = fileno_to_socket[fileno]

                if eventmask & select.EPOLLRDNORM:
                    while True:
                        try:
                            recv_data, address = receiver.recvfrom(1024)
                            sender = addr_to_socket[address]
                            recv_hashes.setdefault((sender.fileno(),
                                receiver.fileno()), hashlib.sha256()).update(
                                        f'<{recv_data}>'.encode('utf-8'))
                            nr_recv = nr_recv + 1
                        except BlockingIOError:
                            break

        # exercise net/rds/tcp.c:rds_tcp_sysctl_reset()
        for net in netns_list:
            ip(f"netns exec {net} /usr/sbin/sysctl net.rds.tcp.rds_tcp_rcvbuf=10000")
            ip(f"netns exec {net} /usr/sbin/sysctl net.rds.tcp.rds_tcp_sndbuf=10000")

    print("done", nr_send, nr_recv)

    # the Python socket module doesn't know these
    RDS_INFO_FIRST = 10000
    RDS_INFO_LAST = 10017

    nr_success = 0
    nr_error = 0

    for s in sockets:
        for optname in range(RDS_INFO_FIRST, RDS_INFO_LAST + 1):
            # Sigh, the Python socket module doesn't allow us to pass
            # buffer lengths greater than 1024 for some reason. RDS
            # wants multiple pages.
            try:
                s.getsockopt(socket.SOL_RDS, optname, 1024)
                nr_success = nr_success + 1
            except OSError as ose:
                nr_error = nr_error + 1
                if ose.errno == errno.ENOSPC:
                    # ignore
                    pass

    print(f"getsockopt(): {nr_success}/{nr_error}")

    # We're done sending and receiving stuff, now let's check if what
    # we received is what we sent.
    for (sender, receiver), send_hash in send_hashes.items():
        recv_hash = recv_hashes.get((sender, receiver))

        if recv_hash is None:
            print("FAIL: No data received")
            return 1

        if send_hash.hexdigest() != recv_hash.hexdigest():
            print("FAIL: Send/recv mismatch")
            print("hash expected:", send_hash.hexdigest())
            print("hash received:", recv_hash.hexdigest())
            return 1

        print(f"{sender}/{receiver}: ok")

    print("Success")
    return 0
