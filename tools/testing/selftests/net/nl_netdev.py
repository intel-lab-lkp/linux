#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from lib.py import ksft_run, ksft_pr, ksft_eq, ksft_ge, NetdevFamily


nf = NetdevFamily()


def empty_check() -> None:
    global nf
    devs = nf.dev_get({}, dump=True)
    ksft_ge(len(devs), 1)


def lo_check() -> None:
    global nf
    lo_info = nf.dev_get({"ifindex": 1})
    ksft_eq(len(lo_info['xdp-features']), 0)
    ksft_eq(len(lo_info['xdp-rx-metadata-features']), 0)


if __name__ == "__main__":
    ksft_run([empty_check, lo_check])
