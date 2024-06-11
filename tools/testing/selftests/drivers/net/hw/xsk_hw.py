#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Run the tools/testing/selftests/net/xsk_hw testsuite."""

from os import path

from lib.py import ksft_run, ksft_exit, KsftSkipEx
from lib.py import NetdevFamily, NetDrvEpEnv
from lib.py import bkg, cmd, ip, wait_port_listen


def build_shared_args(cfg, ipv4):
    """Build common arguments between test cases."""
    if ipv4:
        proto, local_addr, remote_addr = "-4", cfg.v4, cfg.remote_v4
    else:
        proto, local_addr, remote_addr = "-6", cfg.v6, cfg.remote_v6

    local_args = (f"{proto} -i {cfg.ifname} -S {local_addr} -D {remote_addr} "
                     f"-m ${cfg.mac_local} -M ${cfg.mac_nexthop_local}")
    remote_args = (f"{proto} -i {cfg.ifname} -S {remote_addr} -D {local_addr} "
                      f"-m ${cfg.mac_remote} -M ${cfg.mac_nexthop_remote}")

    return local_args, remote_args


def test_receive(cfg, ipv4, extra_args):
    """Test local XSK receive. Remote host sends crafted packets."""
    local_args, remote_args = build_shared_args(cfg, ipv4)

    rx_cmd = f"{cfg.bin_local} -h {local_args} {extra_args}"
    tx_cmd = f"{cfg.bin_remote} {remote_args}"

    with bkg(rx_cmd, exit_wait=True, fail=True):
        wait_port_listen(8000, proto="udp")
        cmd(tx_cmd, host=cfg.remote)


def test_transmit(cfg, ipv4, extra_args):
    """Test local XSK transmit. Remote host verifies packets."""
    local_args, remote_args = build_shared_args(cfg, ipv4)

    rx_cmd = f"{cfg.bin_remote} -h -i {cfg.ifname} {remote_args}"
    tx_cmd = f"{cfg.bin_local} -i {cfg.ifname} {local_args} {extra_args}"

    with bkg(rx_cmd, host=cfg.remote, exit_wait=True, fail=True):
        wait_port_listen(8000, proto="udp", host=cfg.remote)
        cmd(tx_cmd)


def test_builder(name, cfg, ipv4, tx, extra_args="", required_features=()):
    """Construct specific tests from the common template.

       Most tests follow the same basic pattern, differing only in
       the direction of the test, the required XDP features, and
       flags passed to xsk_hw."""
    def f(cfg):
        if ipv4:
            cfg.require_v4()
        else:
            cfg.require_v6()

        if not cfg.have_xdp_features.issuperset(required_features):
            raise KsftSkipEx(f"Test requires XDP features {required_features}, "
                             f"got: {cfg.have_xdp_features}")

        if tx:
            test_transmit(cfg, ipv4, extra_args)
        else:
            test_receive(cfg, ipv4, extra_args)

    if ipv4:
        f.__name__ = "ipv4_" + name
    else:
        f.__name__ = "ipv6_" + name
    return f


def check_nic_features(cfg) -> None:
    """Populate device supported XDP features from netdev netlink.

       If the device does not support any of the required features, then skip
       the relevant tests."""
    features = NetdevFamily().dev_get({"ifindex": cfg.ifindex})
    cfg.have_xdp_features = features["xdp-features"]


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        check_nic_features(cfg)

        cfg.bin_local = path.abspath(path.dirname(__file__) + "/../../../bpf/xsk_hw")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        cfg.mac_local = cfg.dev["address"]
        cfg.mac_remote = ip(f"link show dev {cfg.ifname}",
                            host=cfg.remote, json=True)[0]["address"]

        cfg.mac_nexthop_local = cfg.env["LOCAL_NEXTHOP_MAC"]
        cfg.mac_nexthop_remote = cfg.env["REMOTE_NEXTHOP_MAC"]

        cases = []
        for ipv4 in [True, False]:
            # Basic test with AF_PACKET on both ends
            cases.append(test_builder("basic", cfg, ipv4, False))

            cases.append(test_builder("tx_skb_copy", cfg, ipv4, True, "-T -s -c"))
            cases.append(test_builder("tx_skb_copy_force_attach", cfg, ipv4, True, "-TT -s -c"))
            cases.append(test_builder("rx_skb_copy", cfg, ipv4, False, "-R -s -c"))

            cases.append(test_builder("tx_drv_copy", cfg, ipv4, True, "-T -d -c",
                                      {"basic", "ndo-xmit"}))
            cases.append(test_builder("tx_drv_copy_force_attach", cfg, ipv4, True, "-TT -d -c",
                                      {"basic", "ndo-xmit"}))
            cases.append(test_builder("rx_drv_copy", cfg, ipv4, False, "-R -d -c",
                                      {"basic", "redirect"}))

            cases.append(test_builder("tx_drv_zerocopy", cfg, ipv4, True, "-T -d -z",
                                      {"basic", "xsk-zerocopy", "ndo-xmit"}))
            cases.append(test_builder("tx_drv_zerocopy_force_attach", cfg, ipv4, True, "-TT -d -z",
                                      {"basic", "xsk-zerocopy", "ndo-xmit"}))
            cases.append(test_builder("rx_drv_zerocopy", cfg, ipv4, False, "-R -d -z",
                                      {"basic", "xsk-zerocopy", "redirect"}))
            cases.append(test_builder("rx_drv_zerocopy_fill_after_bind", cfg, ipv4, False, "-R -d -z -f",
                                      {"basic", "xsk-zerocopy", "redirect"}))

        ksft_run(cases=cases, args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
