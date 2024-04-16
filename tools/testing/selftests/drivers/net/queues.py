#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from lib.py import ksft_run, ksft_in, ksft_true, ksft_eq, KsftSkipEx, KsftXfailEx
from lib.py import NetdevFamily, NlError
from lib.py import NetDrvEnv
from lib.py import cmd
import glob

netnl = NetdevFamily()


def sys_get_queues(ifname) -> int:
    folders = glob.glob(f'/sys/class/net/{ifname}/queues/rx-*')
    return len(folders)


def nl_get_queues(cfg):
    global netnl
    queues = netnl.queue_get({'ifindex': cfg.ifindex}, dump=True)
    if queues:
        return len([q for q in queues if q['type'] == 'rx'])
    return None


def get_queues(cfg) -> None:
    global netnl

    queues = nl_get_queues(cfg)
    if not queues:
        raise KsftSkipEx("queue-get not supported by device")

    expected = sys_get_queues(cfg.dev['ifname'])
    ksft_eq(queues, expected)


def addremove_queues(cfg) -> None:
    global netnl

    queues = nl_get_queues(cfg)
    if not queues:
        raise KsftSkipEx("queue-get not supported by device")

    expected = sys_get_queues(cfg.dev['ifname'])
    ksft_eq(queues, expected)

    # reduce queue count by 1
    expected = expected - 1
    cmd(f"ethtool -L {cfg.dev['ifname']} combined {expected}")
    queues = nl_get_queues(cfg)
    ksft_eq(queues, expected)

    # increase queue count by 1
    expected = expected + 1
    cmd(f"ethtool -L {cfg.dev['ifname']} combined {expected}")
    queues = nl_get_queues(cfg)
    ksft_eq(queues, expected)


def main() -> None:
    with NetDrvEnv(__file__, queue_count=3) as cfg:
        cfg.dev_up()
        ksft_run([get_queues, addremove_queues], args=(cfg, ))


if __name__ == "__main__":
    main()
