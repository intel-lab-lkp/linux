#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from lib.py import (Iperf3Runner, NetDrvEpEnv, SystemMonitor, ksft_exit,
                    ksft_run)


def test_iperf(cfg):
    """Measure TCP throughput while sampling system utilization."""
    with SystemMonitor(cfg):
        Iperf3Runner(cfg).measure_bandwidth()


def main():
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        ksft_run([test_iperf], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
