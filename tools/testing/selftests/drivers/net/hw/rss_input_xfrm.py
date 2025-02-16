#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from lib.py import ksft_run, ksft_exit, ksft_eq, ksft_pr
from lib.py import NetDrvEpEnv
from lib.py import EthtoolFamily, NetdevFamily
from lib.py import KsftSkipEx
from lib.py import rand_port, check_port_available_remote
from lib.py import GenerateTraffic
from rss_ctx import _get_rx_cnts


def _get_active_rx_queue(cnts):
    return cnts.index(max(cnts))


def _get_rand_port(remote):
    for _ in range(1000):
        port = rand_port()
        try:
            check_port_available_remote(port, remote)
            return port
        except:
            continue

    raise Exception("Can't find any free unprivileged port")


def test_rss_input_xfrm(cfg):
    """
    Test symmetric input_xfrm.
    If symmetric RSS hash is configured, send traffic twice, swapping the
    src/dst TCP ports, and verify that the same queue is receiving the traffic
    in both cases (IPs are constant).
    """

    input_xfrm = cfg.ethnl.rss_get(
        {'header': {'dev-name': cfg.ifname}}).get('input_xfrm')

    # Check for symmetric xor/or-xor
    if input_xfrm and (input_xfrm == 1 or input_xfrm == 2):
        port1 = _get_rand_port(cfg.remote)
        port2 = _get_rand_port(cfg.remote)
        ksft_pr(f'Running traffic on ports: {port1 = }, {port2 = }')

        cnts = _get_rx_cnts(cfg)
        GenerateTraffic(cfg, port=port1, parallel=1,
                        cport=port2).wait_pkts_and_stop(20000)
        cnts = _get_rx_cnts(cfg, prev=cnts)
        rxq1 = _get_active_rx_queue(cnts)
        ksft_pr(f'Received traffic on {rxq1 = }')

        cnts = _get_rx_cnts(cfg)
        GenerateTraffic(cfg, port=port2, parallel=1,
                        cport=port1).wait_pkts_and_stop(20000)
        cnts = _get_rx_cnts(cfg, prev=cnts)
        rxq2 = _get_active_rx_queue(cnts)
        ksft_pr(f'Received traffic on {rxq2 = }')

        ksft_eq(
            rxq1, rxq2, comment=f"Received traffic on different queues ({rxq1} != {rxq2}) while symmetric hash is configured")
    else:
        raise KsftSkipEx("Symmetric RSS hash not requested")


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        cfg.netdevnl = NetdevFamily()

        ksft_run([test_rss_input_xfrm],
                 args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
