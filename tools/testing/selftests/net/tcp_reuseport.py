#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import os

from lib.py import ksft_run, ksft_exit
from socket import *

def test_reuseport_select() -> None:
    s1 = socket()
    s1.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1)
    s1.setsockopt(SOL_SOCKET, SO_BINDTODEVICE, b'lo')
    s1.listen()
    s1.setblocking(False)

    s2 = socket()
    s2.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1)
    s2.bind(s1.getsockname())
    s2.listen()
    s2.setblocking(False)

    for i in range(3):
        c = socket()
        c.connect(s1.getsockname())
        try:
            print("SUCCESS: assigned properly:", s1.accept())
        except:
            print("FAIL: wrong assignment")
            os.sys.exit(1)

def main() -> None:
    ksft_run([test_reuseport_select])
    ksft_exit()

if __name__ == "__main__":
    main()
