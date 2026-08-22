#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Helpers shared by the PSP tests.

Only code which the tests would otherwise have to copy verbatim belongs
here, mostly talking to psp_responder on the other end of the link.
"""

import fcntl
import socket
import struct
import termios
import time
from contextlib import contextmanager

from lib.py import defer
from lib.py import ksft_eq, ksft_pr
from lib.py import KsftSkipEx, KsftFailEx
from lib.py import bkg, rand_port, wait_port_listen


def get_outq(s):
    one = b'\0' * 4
    outq = fcntl.ioctl(s.fileno(), termios.TIOCOUTQ, one)
    return struct.unpack("I", outq)[0]


def send_with_ack(cfg, msg):
    cfg.comm_sock.send(msg)
    response = cfg.comm_sock.recv(4)
    if response != b'ack\0':
        raise RuntimeError("Unexpected server response", response)


def remote_read_len(cfg):
    cfg.comm_sock.send(b'read len\0')
    return int(cfg.comm_sock.recv(1024)[:-1].decode('utf-8'))


def make_clr_conn(cfg, ipver=None):
    send_with_ack(cfg, b'conn clr\0')
    remote_addr = cfg.remote_addr_v[ipver] if ipver else cfg.remote_addr
    s = socket.create_connection((remote_addr, cfg.comm_port), )
    return s


def make_psp_conn(cfg, version=0, ipver=None):
    send_with_ack(cfg, b'conn psp\0' + struct.pack('BB', version, version))
    remote_addr = cfg.remote_addr_v[ipver] if ipver else cfg.remote_addr
    s = socket.create_connection((remote_addr, cfg.comm_port), )
    return s


def close_conn(cfg, s):
    send_with_ack(cfg, b'data close\0')
    s.close()


def remote_dev_steer(cfg, mode):
    """Set vc-steer-ena on the remote PSP device"""
    send_with_ack(cfg, b'dev steer\0' + struct.pack('B', mode))


def spi_xchg(s, rx):
    s.send(struct.pack('I', rx['spi']) + rx['key'])
    tx = s.recv(4 + len(rx['key']))
    return {
        'spi': struct.unpack('I', tx[:4])[0],
        'key': tx[4:]
    }


def send_careful(cfg, s, rounds):
    data = b'0123456789' * 200
    for i in range(rounds):
        n = 0
        for _ in range(10): # allow 10 retries
            try:
                n += s.send(data[n:], socket.MSG_DONTWAIT)
                if n == len(data):
                    break
            except BlockingIOError:
                time.sleep(0.05)
        else:
            rlen = remote_read_len(cfg)
            outq = get_outq(s)
            report = f'sent: {i * len(data) + n} remote len: {rlen} outq: {outq}'
            raise RuntimeError(report)

    return len(data) * rounds


def check_data_rx(cfg, exp_len):
    read_len = -1
    for _ in range(30):
        cfg.comm_sock.send(b'read len\0')
        read_len = int(cfg.comm_sock.recv(1024)[:-1].decode('utf-8'))
        if read_len == exp_len:
            break
        time.sleep(0.01)
    ksft_eq(read_len, exp_len)


def get_stat(cfg, key):
    return cfg.pspnl.get_stats({'dev-id': cfg.psp_dev_id})[key]

def init_psp_dev(cfg, use_psp_ifindex=False):
    if not hasattr(cfg, 'psp_dev_id'):
        # Figure out which local device we are testing against
        # For NetDrvContEnv: use psp_ifindex instead of ifindex
        target_ifindex = cfg.psp_ifindex if use_psp_ifindex else cfg.ifindex
        for dev in cfg.pspnl.dev_get({}, dump=True):
            if dev['ifindex'] == target_ifindex:
                cfg.psp_info = dev
                cfg.psp_dev_id = cfg.psp_info['id']
                break
        else:
            raise KsftSkipEx("No PSP devices found")

    # Enable PSP if necessary
    cap = cfg.psp_info['psp-versions-cap']
    ena = cfg.psp_info['psp-versions-ena']
    if cap != ena:
        cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'psp-versions-ena': cap})
        defer(cfg.pspnl.dev_set, {'id': cfg.psp_dev_id,
                                  'psp-versions-ena': ena })


def recv_careful(s, target, rounds=100):
    """Read exactly target bytes, tolerating short reads"""
    data = b''
    for _ in range(rounds):
        try:
            data += s.recv(target - len(data), socket.MSG_DONTWAIT)
            if len(data) == target:
                return data
        except BlockingIOError:
            time.sleep(0.001)
    raise KsftFailEx(f"short read, got {len(data)} of {target} bytes")


def req_echo(cfg, s):
    """Ask the peer to echo, and check the reply arrives intact"""
    send_with_ack(cfg, b'data echo\0')
    ksft_eq(recv_careful(s, 5), b'echo\0')


def psp_txrx(cfg, s, rounds, sent=0):
    """Send data both ways, and return the total bytes sent to the peer"""
    sent += send_careful(cfg, s, rounds)
    check_data_rx(cfg, sent)
    req_echo(cfg, s)
    return sent


@contextmanager
def responder(cfg):
    """Run psp_responder on the remote end and open the comm socket to it"""
    binary = cfg.remote.deploy("psp_responder")

    cfg.comm_port = rand_port()
    srv = None
    try:
        with bkg(binary + f" -p {cfg.comm_port} -i {cfg.remote_ifindex}",
                 host=cfg.remote, exit_wait=True) as srv:
            wait_port_listen(cfg.comm_port, host=cfg.remote)

            cfg.comm_sock = socket.create_connection((cfg.remote_addr,
                                                      cfg.comm_port),
                                                     timeout=1)
            yield cfg

            cfg.comm_sock.send(b"exit\0")
            cfg.comm_sock.close()
    finally:
        if srv and (srv.stdout or srv.stderr):
            ksft_pr("")
            ksft_pr(f"Responder logs ({srv.ret}):")
        if srv and srv.stdout:
            ksft_pr("STDOUT:\n#  " + srv.stdout.strip().replace("\n", "\n#  "))
        if srv and srv.stderr:
            ksft_pr("STDERR:\n#  " + srv.stderr.strip().replace("\n", "\n#  "))
