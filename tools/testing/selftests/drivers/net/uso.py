#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Test USO

Sends large UDP datagrams with UDP_SEGMENT and verifies that the peer
receives the correct number of individual segments with correct sizes.
"""
import socket
import time

from lib.py import ksft_run, ksft_exit, KsftSkipEx
from lib.py import ksft_ge
from lib.py import NetDrvEpEnv
from lib.py import defer, ethtool, ip, rand_port

# python doesn't expose this constant, so we need to hardcode it to enable UDP
# segmentation for large payloads
UDP_SEGMENT = 103


def _send_uso(cfg, ipver, mss, total_payload, port):
    if ipver == "4":
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dst = (cfg.remote_addr_v["4"], port)
    else:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        dst = (cfg.remote_addr_v["6"], port)

    sock.setsockopt(socket.IPPROTO_UDP, UDP_SEGMENT, mss)
    payload = bytes(range(256)) * ((total_payload // 256) + 1)
    payload = payload[:total_payload]
    sock.sendto(payload, dst)
    sock.close()
    return payload


def _get_rx_packets(cfg):
    stats = ip(f"-s link show dev {cfg.remote_ifname}",
               json=True, host=cfg.remote)[0]
    return stats['stats64']['rx']['packets']


def _test_uso(cfg, ipver, mss, total_payload):
    cfg.require_ipver(ipver)

    try:
        ethtool(f"-K {cfg.ifname} tx-udp-segmentation on")
    except Exception as exc:
        raise KsftSkipEx(
            "Device does not support tx-udp-segmentation") from exc
    defer(ethtool, f"-K {cfg.ifname} tx-udp-segmentation off")

    expected_segs = (total_payload + mss - 1) // mss

    rx_before = _get_rx_packets(cfg)

    port = rand_port(stype=socket.SOCK_DGRAM)
    _send_uso(cfg, ipver, mss, total_payload, port)

    time.sleep(0.5)

    rx_after = _get_rx_packets(cfg)
    rx_delta = rx_after - rx_before

    ksft_ge(rx_delta, expected_segs,
            comment=f"Expected >= {expected_segs} rx packets, got {rx_delta}")


def test_uso_v4(cfg):
    """USO IPv4: 11 segments (10 full + 1 partial)."""
    _test_uso(cfg, "4", 1400, 1400 * 10 + 500)


def test_uso_v6(cfg):
    """USO IPv6: 11 segments (10 full + 1 partial)."""
    _test_uso(cfg, "6", 1400, 1400 * 10 + 500)


def test_uso_v4_exact(cfg):
    """USO IPv4: exact multiple of MSS (5 full segments)."""
    _test_uso(cfg, "4", 1400, 1400 * 5)


def main() -> None:
    """Run USO tests."""
    with NetDrvEpEnv(__file__) as cfg:
        ksft_run([test_uso_v4,
                  test_uso_v6,
                  test_uso_v4_exact],
                 args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
