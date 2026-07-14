#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023-2026  David 'equinox' Lamparter
"""
RFC 6724 (IPv6 source address selection) rule 5.5 tests

For reference:

   Rule 5.5: Prefer addresses in a prefix advertised by the next-hop.
   If SA or SA's prefix is assigned by the selected next-hop that will
   be used to send to D and SB or SB's prefix is assigned by a different
   next-hop, then prefer SA.  Similarly, if SB or SB's prefix is
   assigned by the next-hop that will be used to send to D and SA or
   SA's prefix is assigned by a different next-hop, then prefer SB.

(and since it provides the "counterpoint":)

   Rule 8: Use longest matching prefix.
   If CommonPrefixLen(SA, D) > CommonPrefixLen(SB, D), then prefer SA.
   Similarly, if CommonPrefixLen(SB, D) > CommonPrefixLen(SA, D), then
   prefer SB.

Note rule 5.5 was originally optional but made mandatory by
draft-ietf-6man-rfc6724-update (which at the point of creation of this test
was already "done" at the IETF but waiting in the RFC editor queue due to a
blocking dependency.)
"""

from socket import socket, AF_INET6, SOCK_DGRAM
from functools import wraps
from typing import Callable

from lib.py import ksft_run, ksft_exit, ksft_eq
from lib.py import NetNS, NetNSEnter
from lib.py import ip


def select_addr(dest):
    """
    connect() + getsockname() to figure out what was selected as source address
    """
    sock = socket(AF_INET6, SOCK_DGRAM, 0)
    sock.connect((dest, 12345))
    return sock.getsockname()[0]


def in_netns(func: Callable[[], None]) -> Callable[[], None]:
    """
    python decorator to put test function in netns
    """

    @wraps(func)
    def wrapped() -> None:
        with NetNS() as testns:
            with NetNSEnter(str(testns)):
                func()

    return wrapped


@in_netns
def test_basic() -> None:
    """
    Simple & most common case for RFC6724 rule 5.5: multiple default routes
    """
    ip("link add type veth")
    ip("link set veth0 up")
    ip("link set veth1 up")
    ip("addr add 2001:db8:10::1/64   dev veth0 nodad")
    ip("addr add 2001:db8:1000::1/64 dev veth0 nodad")
    ip("-6 route add default via fe80::1 dev veth0 metric 100")
    ip("-6 route add default via fe80::2 dev veth0 metric 200")
    ip("-6 route add default from 2001:db8:10::/48   via fe80::1 dev veth0")
    ip("-6 route add default from 2001:db8:1000::/48 via fe80::2 dev veth0")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    # rule 8 would result in the use of the :1001: address, but rule 5.5 applies before.
    ksft_eq(select_addr("2001:db8:1001::"), "2001:db8:10::1", "rule 5.5 > rule 8")

    ip("-6 route del default via fe80::1 dev veth0 metric 100")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:1000::1", "baseline pass")
    ksft_eq(select_addr("2001:db8:1001::"), "2001:db8:1000::1", "rule 5.5 > rule 8")


@in_netns
def test_nh_obj() -> None:
    """
    Same as above, but with nexthop objects for the default route

    NB: The kernel doesn't currently allow nexthop objects for subtree routes.
    """

    ip("link add type veth")
    ip("link set veth0 up")
    ip("link set veth1 up")
    ip("addr add 2001:db8:10::1/64   dev veth0 nodad")
    ip("addr add 2001:db8:1000::1/64 dev veth0 nodad")

    # distinct nexthop objects are used, because what matters is the nexthop
    # itself, not the nexthop object.  To cover everything, make a group.
    ip("nexthop add id 101 via fe80::1 dev veth0")
    ip("nexthop add id 201 group 101")
    ip("nexthop add id 102 via fe80::2 dev veth0")
    ip("nexthop add id 202 group 102")

    ip("-6 route add default nhid 201 metric 100")
    ip("-6 route add default nhid 202 metric 200")
    ip("-6 route add default from 2001:db8:10::/48   via fe80::1 dev veth0")
    ip("-6 route add default from 2001:db8:1000::/48 via fe80::2 dev veth0")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    # rule 8 would result in the use of the :1001: address, but rule 5.5 applies before.
    ksft_eq(select_addr("2001:db8:1001::"), "2001:db8:10::1", "rule 5.5 > rule 8")

    ip("-6 route del default nhid 201 metric 100")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:1000::1", "baseline pass")
    ksft_eq(select_addr("2001:db8:1001::"), "2001:db8:1000::1", "rule 5.5 > rule 8")


@in_netns
def test_low_metric() -> None:
    """
    Check that subtree routes take effect even if they are higher metric

    For checking that "router advertised prefix", metric is irrelevant.  It
    matters for the initial unspecific lookup to find a nexthop to begin with.
    (The later source address check lookup doesn't change the nexthop, i.e.
    the effects of metrics are already done.)
    """
    ip("link add type veth")
    ip("link set veth0 up")
    ip("link set veth1 up")
    ip("addr add 2001:db8:10::1/64   dev veth0 nodad")
    ip("addr add 2001:db8:1000::1/64 dev veth0 nodad")
    ip("-6 route add default via fe80::1 dev veth0 metric 100")
    ip("-6 route add default via fe80::2 dev veth0 metric 200")
    ip("-6 route add default from 2001:db8:10::/48   via fe80::1 dev veth0")
    ip("-6 route add default from 2001:db8:1000::/48 via fe80::2 dev veth0 metric 1000")
    ip("-6 route add default from 2001:db8:1000::/48 via fe80::3 dev veth0 metric 50")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    # rule 8 would result in the use of the :1001: address, but rule 5.5 applies before.
    ksft_eq(select_addr("2001:db8:1001::"), "2001:db8:10::1", "rule 5.5 > rule 8")

    ip("-6 route del default via fe80::1 dev veth0 metric 100")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:1000::1", "baseline pass")
    ksft_eq(select_addr("2001:db8:1001::"), "2001:db8:1000::1", "rule 5.5 > rule 8")


@in_netns
def test_no_subtree() -> None:
    """
    Ensure that matching on a non-subtree route doesn't trigger rule 5.5

    (This was non-obviously broken in earlier versions of the implementations,
    a non-subtree route would still match.  Make sure it doesn't break again.)
    """
    ip("link add type veth")
    ip("link set veth0 up")
    ip("link set veth1 up")
    ip("addr add 2001:db8:10::1/64 dev veth0 nodad")
    ip("addr add 2001:db8:1000::1/64 dev veth0 nodad")
    ip("-6 route add default via fe80::1 dev veth0 metric 100")
    ip("-6 route add default via fe80::2 dev veth0 metric 200")
    ip("-6 route add default from 2001:db8:10::/48 via fe80::1 dev veth0")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    ksft_eq(
        select_addr("2001:db8:1001::"),
        "2001:db8:10::1",
        "rule 5.5 > rule 8, ignoring non-SADR",
    )


@in_netns
def test_longer() -> None:
    """
    Check functionality for non-default destination.

    This is expected to be very rare in actual practice, and doesn't do
    backtracking (also refer to kernel docs.)
    """
    ip("link add type veth")
    ip("link set veth0 up")
    ip("link set veth1 up")
    ip("addr add 2001:db8:10::1/64 dev veth0 nodad")
    ip("addr add 2001:db8:1000::1/64 dev veth0 nodad")
    ip("-6 route add default           via fe80::1 dev veth0 metric 100")
    ip("-6 route add 2001:db8:500::/48 via fe80::2 dev veth0 metric 100")
    ip("-6 route add default           from 2001:db8:10::/48   via fe80::1 dev veth0")
    ip("-6 route add 2001:db8:500::/48 from 2001:db8:1000::/48 via fe80::2 dev veth0")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    ksft_eq(select_addr("2001:db8:500::"), "2001:db8:1000::1", "rule 5.5")
    ksft_eq(select_addr("2001:db8:500:aaa::"), "2001:db8:1000::1", "rule 5.5")

    ip("-6 route add 2001:db8:500:aaa::/64 via fe80::2 dev veth0 metric 100")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    ksft_eq(select_addr("2001:db8:500::"), "2001:db8:1000::1", "rule 5.5")
    ksft_eq(select_addr("2001:db8:500:aaa::"), "2001:db8:10::1", "no backtracking")

    ip("-6 route del 2001:db8:500::/48 from 2001:db8:1000::/48 via fe80::2 dev veth0")
    ip("-6 route add default           from 2001:db8:1000::/48 via fe80::2 dev veth0")

    ksft_eq(select_addr("2001:db8:11::"), "2001:db8:10::1", "baseline pass")
    ksft_eq(select_addr("2001:db8:500::"), "2001:db8:10::1", "no backtracking")
    ksft_eq(select_addr("2001:db8:500:aaa::"), "2001:db8:10::1", "no backtracking")


def main() -> None:
    """
    RFC6724 rule 5.5 test driver
    """
    ksft_run(
        [
            test_basic,
            test_nh_obj,
            test_low_metric,
            test_no_subtree,
            test_longer,
        ]
    )
    ksft_exit()


if __name__ == "__main__":
    main()
