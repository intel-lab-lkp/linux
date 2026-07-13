#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Copyright (C) 2026 David 'equinox' Lamparter

Linux kernel self-test for AF_LLC sockets, specifically jumbo frame (802.1AC)
support.
"""

from __future__ import annotations

import os
import signal
import struct
import binascii
import functools
import errno
import ctypes
from typing import Callable

from socket import socket, htons, SOCK_DGRAM, SOCK_RAW, AF_PACKET, ETH_P_ALL

from lib.py import ksft_run, ksft_exit, ksft_eq, KsftSkipEx
from lib.py import ip

# from lib.py import NetNS, NetNSEnter


ETH_P_8021AC = 0x8870
AF_LLC = 26


class ContextSkip:
    """
    convert exception from context manager into KsftSkipEx with message
    """

    def __init__(self, ctx, message):
        self._ctx = ctx
        self._message = message

    def __enter__(self):
        try:
            return self._ctx.__enter__()
        except Exception as e:
            raise KsftSkipEx(self._message + "[" + repr(e) + "]") from e

    def __exit__(self, exc_type, exc_value, traceback):
        self._ctx.__exit__(exc_type, exc_value, traceback)


# Python's socket.bind() has no clue about AF_LLC
c_macaddr = ctypes.c_ubyte * 6


class sockaddr_llc(ctypes.Structure):
    _fields_ = [
        ("sllc_family", ctypes.c_ushort),
        ("sllc_arphrd", ctypes.c_ushort),
        ("sllc_test", ctypes.c_ubyte),
        ("sllc_xid", ctypes.c_ubyte),
        ("sllc_ua", ctypes.c_ubyte),
        ("sllc_sap", ctypes.c_ubyte),
        ("sllc_mac", c_macaddr),
        ("pad", ctypes.c_ubyte * 2),
    ]

    @classmethod
    def make(
        cls,
        sllc_family=AF_LLC,
        sllc_arphrd=0,
        sllc_test=0,
        sllc_xid=0,
        sllc_ua=0,
        sllc_sap=0,
        sllc_mac: None | str | bytes = None,
    ) -> sockaddr_llc:
        if sllc_mac is None:
            _sllc_mac = c_macaddr(0, 0, 0, 0, 0, 0)
        elif isinstance(sllc_mac, str):
            _sllc_mac = c_macaddr(*(int(b, 16) for b in sllc_mac.split(":")))
        else:
            _sllc_mac = c_macaddr(*(b for b in sllc_mac))
        return cls(
            sllc_family, sllc_arphrd, sllc_test, sllc_xid, sllc_ua, sllc_sap, _sllc_mac
        )


assert len(bytes(sockaddr_llc())) == 16


libc = ctypes.CDLL(None, use_errno=True)
libc.bind.argtypes = (ctypes.c_int, ctypes.POINTER(sockaddr_llc), ctypes.c_int)
libc.bind.restype = ctypes.c_int
libc.sendto.argtypes = (
    ctypes.c_int,
    ctypes.c_voidp,
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.POINTER(sockaddr_llc),
    ctypes.c_int,
)
libc.sendto.restype = ctypes.c_int


def llc_bind(fd: socket, addr: sockaddr_llc) -> None:
    ret = libc.bind(fd.fileno(), addr, len(bytes(addr)))
    if ret:
        err = ctypes.get_errno()
        raise OSError(err, errno.errorcode.get(err, str(err)))


def llc_sendto(fd: socket, addr: sockaddr_llc, data: bytes, flags=0) -> None:
    ret = libc.sendto(fd.fileno(), data, len(data), flags, addr, len(bytes(addr)))
    if ret < 0:
        err = ctypes.get_errno()
        raise OSError(err, errno.errorcode.get(err, str(err)))
    return ret


def wrap_common_setup(testfn: Callable[[socket, socket], None]) -> Callable[[], None]:
    """
    common setup for all LLC tests

    (create netns, create AF_LLC + AF_PACKET sockets)
    """

    def inner1() -> None:
        if os.path.exists("/sys/class/net/testveth0"):
            raise KsftSkipEx("already have a testveth0 netdev")
        if os.path.exists("/sys/class/net/testveth1"):
            raise KsftSkipEx("already have a testveth1 netdev")

        ip(
            "link add name testveth0 address 02:00:00:00:00:00 "
            + "type veth peer name testveth1 address 02:11:11:11:11:11"
        )
        with open(
            "/proc/sys/net/ipv6/conf/testveth0/disable_ipv6", "w", encoding="ASCII"
        ) as fd:
            fd.write("1\n")
        with open(
            "/proc/sys/net/ipv6/conf/testveth1/disable_ipv6", "w", encoding="ASCII"
        ) as fd:
            fd.write("1\n")
        ip("link set testveth0 mtu 9000 up")
        ip("link set testveth1 mtu 9000 up")

        try:
            with ContextSkip(
                socket(AF_LLC, SOCK_DGRAM, 0), "AF_LLC not enabled in kernel?"
            ) as sock0_llc:
                llc_bind(
                    sock0_llc,
                    sockaddr_llc.make(sllc_sap=0xFE, sllc_mac="02:00:00:00:00:00"),
                )

                with ContextSkip(
                    socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)),
                    "AF_PACKET not enabled in kernel?",
                ) as sock1_pkt:
                    sock1_pkt.bind(("testveth1", ETH_P_ALL))

                    signal.alarm(10)
                    testfn(sock0_llc, sock1_pkt)

        finally:
            signal.alarm(0)
            ip("link del testveth0")

    @functools.wraps(testfn)
    def inner() -> None:
        if os.getuid() == 0:
            inner1()
        else:
            raise KsftSkipEx(
                "this test requires root since AF_LLC has no netns support (yet?)"
            )
            # with NetNS() as ns1:
            #    with NetNSEnter(str(ns1)):
            #        inner1()

    return inner


def pkt_check(fd: socket, expectlen: int, data: None | bytes = None):
    # ignore non-LLC packets
    # (since we can't run in netns, random junk may show up)
    proto_len = -1
    while not (0 <= proto_len < 1536 or proto_len == 0x8870):
        pkt = fd.recv(4096)
        proto_len = struct.unpack(">H", pkt[12:14])[0]

    assert binascii.b2a_hex(pkt[0:6]) == b"021111111111"
    assert binascii.b2a_hex(pkt[6:12]) == b"020000000000"

    if expectlen <= 1497:
        ksft_eq(proto_len, expectlen + 3)
    else:
        ksft_eq(proto_len, ETH_P_8021AC, "802.1AC ethertype (0x8870)")

    if data is None:
        data = b"\00" * expectlen
    else:
        data = (data + b"\00" * expectlen)[:expectlen]

    ksft_eq(binascii.b2a_hex(pkt[14:17]).decode("ASCII").upper(), "FEFE03")
    ksft_eq(pkt[17:], data)


def test_llc_tx(sock0_llc: socket, sock1_pkt: socket) -> None:
    llc_sendto(
        sock0_llc,
        sockaddr_llc.make(sllc_sap=0xFE, sllc_mac="02:11:11:11:11:11"),
        b"\x00" * 1400,
    )
    pkt_check(sock1_pkt, 1400)

    llc_sendto(
        sock0_llc,
        sockaddr_llc.make(sllc_sap=0xFE, sllc_mac="02:11:11:11:11:11"),
        b"\x00" * 1497,
    )
    pkt_check(sock1_pkt, 1497)


def test_llc_tx_jumbo(sock0_llc: socket, sock1_pkt: socket) -> None:
    llc_sendto(
        sock0_llc,
        sockaddr_llc.make(sllc_sap=0xFE, sllc_mac="02:11:11:11:11:11"),
        b"\x00" * 1498,
    )
    pkt_check(sock1_pkt, 1498)

    llc_sendto(
        sock0_llc,
        sockaddr_llc.make(sllc_sap=0xFE, sllc_mac="02:11:11:11:11:11"),
        b"\x00" * 4000,
    )
    pkt_check(sock1_pkt, 4000)


machdr = binascii.a2b_hex("020000000000" + "021111111111")


def test_llc_rx(sock0_llc: socket, sock1_pkt: socket) -> None:
    sock1_pkt.send(
        machdr + struct.pack(">H", 1403) + binascii.a2b_hex("FEFE03") + b"\x00" * 1400
    )
    rxdata = sock0_llc.recv(4096)
    ksft_eq(len(rxdata), 1400)
    ksft_eq(rxdata, b"\x00" * 1400)

    sock1_pkt.send(
        machdr + struct.pack(">H", 1500) + binascii.a2b_hex("FEFE03") + b"\x00" * 1497
    )
    rxdata = sock0_llc.recv(4096)
    ksft_eq(len(rxdata), 1497)
    ksft_eq(rxdata, b"\x00" * 1497)


def test_llc_rx_jumbo(sock0_llc: socket, sock1_pkt: socket) -> None:
    sock1_pkt.send(
        machdr
        + struct.pack(">H", ETH_P_8021AC)
        + binascii.a2b_hex("FEFE03")
        + b"\x00" * 1498
    )
    rxdata = sock0_llc.recv(4096)
    ksft_eq(len(rxdata), 1498)
    ksft_eq(rxdata, b"\x00" * 1498)

    sock1_pkt.send(
        machdr
        + struct.pack(">H", ETH_P_8021AC)
        + binascii.a2b_hex("FEFE03")
        + b"\x00" * 4000
    )
    rxdata = sock0_llc.recv(4096)
    ksft_eq(len(rxdata), 4000)
    ksft_eq(rxdata, b"\x00" * 4000)


def test_llc_rx_reject_smuggling(sock0_llc: socket, sock1_pkt: socket) -> None:
    """
    make sure smaller packets can't be smuggled in using 0x8870 ethertype
    """
    # these tests use a 2nd packet to check first one was dropped, relying on ordering

    sock1_pkt.send(
        machdr
        + struct.pack(">H", ETH_P_8021AC)
        + binascii.a2b_hex("FEFE03")
        + b"\x00" * 1497
    )
    sock1_pkt.send(
        machdr + struct.pack(">H", 131) + binascii.a2b_hex("FEFE03") + b"\x00" * 128
    )
    rxdata = sock0_llc.recv(4096)
    ksft_eq(len(rxdata), 128)
    ksft_eq(rxdata, b"\x00" * 128)

    sock1_pkt.send(
        machdr
        + struct.pack(">H", ETH_P_8021AC)
        + binascii.a2b_hex("FEFE03")
        + b"\x00" * 250
    )
    sock1_pkt.send(
        machdr + struct.pack(">H", 131) + binascii.a2b_hex("FEFE03") + b"\x00" * 128
    )
    rxdata = sock0_llc.recv(4096)
    ksft_eq(len(rxdata), 128)
    ksft_eq(rxdata, b"\x00" * 128)


def main() -> None:
    ksft_run(
        [
            wrap_common_setup(test_llc_tx),
            wrap_common_setup(test_llc_tx_jumbo),
            wrap_common_setup(test_llc_rx),
            wrap_common_setup(test_llc_rx_jumbo),
            wrap_common_setup(test_llc_rx_reject_smuggling),
        ]
    )
    ksft_exit()


def sigalrm(sig, frame):
    raise TimeoutError("SIGALRM")


if __name__ == "__main__":
    signal.signal(signal.SIGALRM, sigalrm)
    main()
