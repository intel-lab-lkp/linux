#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import ctypes

from lib.py import ksft_run, ksft_exit, ksft_true
from lib.py import ip
from lib.py import NetNS
from lib.py import RtnlFamily

libc = ctypes.cdll.LoadLibrary('libc.so.6')


class NSEnter:
    def __init__(self, ns_name):
        self.ns_path = f"/run/netns/{ns_name}"

    def __enter__(self):
        self.saved = open("/proc/thread-self/ns/net")
        with open(self.ns_path) as ns_file:
            libc.setns(ns_file.fileno(), 0)

    def __exit__(self, exc_type, exc_value, traceback):
        libc.setns(self.saved.fileno(), 0)
        self.saved.close()


def test_event(ns1, ns2) -> None:
    with NSEnter(str(ns1)):
        rtnl = RtnlFamily()

    rtnl.ntf_subscribe("rtnlgrp-link")

    ip(f"netns set {ns1} 0", ns=str(ns2))

    ip(f"link add netns {ns2} link-netnsid 0 dummy1 type dummy")
    ip(f"link add netns {ns2} dummy2 type dummy", ns=str(ns1))

    ip("link del dummy1", ns=str(ns2))
    ip("link del dummy2", ns=str(ns2))

    # Should receive no link events in ns1. Wait 5*0.1 seconds.
    ksft_true(next(rtnl.check_ntf(max_retries=5), None) is None,
              "Received unexpected link notification")


def main() -> None:
    with NetNS() as ns1, NetNS() as ns2:
        ksft_run([test_event], args=(ns1, ns2))
    ksft_exit()


if __name__ == "__main__":
    main()
