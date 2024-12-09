#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import time

from lib.py import ksft_run, ksft_exit, ksft_true
from lib.py import ip
from lib.py import NetNS, NetNSEnter
from lib.py import RtnlFamily


def test_event(ns1, ns2) -> None:
    with NetNSEnter(str(ns1)):
        rtnl = RtnlFamily()

    rtnl.ntf_subscribe("rtnlgrp-link")

    ip(f"netns set {ns1} 0", ns=str(ns2))

    ip(f"link add netns {ns2} link-netnsid 0 dummy1 type dummy")
    ip(f"link add netns {ns2} dummy2 type dummy", ns=str(ns1))

    ip("link del dummy1", ns=str(ns2))
    ip("link del dummy2", ns=str(ns2))

    time.sleep(1)
    rtnl.check_ntf()
    ksft_true(rtnl.async_msg_queue.empty(),
              "Received unexpected link notification")


def main() -> None:
    with NetNS() as ns1, NetNS() as ns2:
        ksft_run([test_event], args=(ns1, ns2))
    ksft_exit()


if __name__ == "__main__":
    main()
