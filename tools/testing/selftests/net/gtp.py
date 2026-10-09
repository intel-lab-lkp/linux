#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Tests for the gtp netlink family."""

from lib.py import ksft_run, ksft_exit
from lib.py import ksft_eq, ksft_in, ksft_not_in, ksft_raises
from lib.py import KsftSkipEx
from lib.py import NetNS, NetNSEnter
from lib.py import GtpFamily, NlError
from lib.py import CmdExitFailure
from lib.py import ip, defer


def _add_gtp_dev(ns):
    try:
        ip("link add gtp0 type gtp role ggsn", ns=str(ns))
    except CmdExitFailure:
        raise KsftSkipEx("no gtp support (CONFIG_GTP, iproute2)")
    defer(ip, "link del gtp0", ns=str(ns))
    ip("link set gtp0 up", ns=str(ns))
    return ip("-d link show gtp0", ns=str(ns), json=True)[0]["ifindex"]


def _iteis(contexts):
    return [c.get("i-tei") for c in contexts]


def add_del_pdp(gtp, ns) -> None:
    """Add a PDP context, query it, then delete it.

    The query must return the context with the TEIDs and addresses
    it was created with, and it must be gone after the delete.
    """
    link = _add_gtp_dev(ns)

    gtp.newpdp({"link": link, "version": 1,
                "i-tei": 0x100, "o-tei": 0x200,
                "ms-address": "10.0.0.1", "peer-address": "192.0.2.1"})

    pdp = [c for c in gtp.getpdp({}, dump=True) if c.get("i-tei") == 0x100]
    ksft_eq(len(pdp), 1)
    ksft_eq(pdp[0]["o-tei"], 0x200)
    ksft_eq(pdp[0]["ms-address"], "10.0.0.1")
    ksft_eq(pdp[0]["peer-address"], "192.0.2.1")

    gtp.delpdp({"link": link, "version": 1,
                "i-tei": 0x100, "ms-address": "10.0.0.1"})
    ksft_not_in(0x100, _iteis(gtp.getpdp({}, dump=True)))


def add_two_pdp(gtp, ns) -> None:
    """Add two PDP contexts and check both are returned."""
    link = _add_gtp_dev(ns)

    for i_tei, o_tei, ms in ((0x111, 0x211, "10.0.1.1"),
                             (0x112, 0x212, "10.0.1.2")):
        gtp.newpdp({"link": link, "version": 1,
                    "i-tei": i_tei, "o-tei": o_tei,
                    "ms-address": ms, "peer-address": "192.0.2.5"})

    iteis = _iteis(gtp.getpdp({}, dump=True))
    ksft_in(0x111, iteis)
    ksft_in(0x112, iteis)


def del_unknown_pdp(gtp, ns) -> None:
    """Deleting a context that was never added must fail."""
    link = _add_gtp_dev(ns)
    with ksft_raises(NlError):
        gtp.delpdp({"link": link, "version": 1,
                    "i-tei": 0x999, "ms-address": "10.9.9.9"})


def main() -> None:
    """Run the PDP context tests in a temporary netns."""
    with NetNS() as ns:
        with NetNSEnter(str(ns)):
            gtp = GtpFamily()
        ksft_run([add_del_pdp, add_two_pdp, del_unknown_pdp], args=(gtp, ns))
    ksft_exit()


if __name__ == "__main__":
    main()
