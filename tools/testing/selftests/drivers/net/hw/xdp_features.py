#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Check that a device's advertised XDP features match its behavior."""

import ipaddress
from pathlib import Path
import re
import shlex

from lib.py import bkg, cmd, ksft_eq, ksft_exit, ksft_run, NetDrvEpEnv


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
RESULT = re.compile(
    r"Feature .*: \[(NOT )?DETECTED\]\[(NOT )?ADVERTISED\]"
)


def mapped_address(address):
    """Return an IPv6 or IPv4-mapped IPv6 address for xdp_features."""
    address = ipaddress.ip_address(address)
    if address.version == 4:
        return f"::ffff:{address}"
    return str(address)


def feature_command(binary, feature, dut_addr, tester_addr, ifname,
                    tester=False):
    args = [str(binary)]
    if tester:
        args.append("-t")
    args += ["-f", feature, "-D", dut_addr]
    if tester:
        args += ["-C", dut_addr]
    args += ["-T", tester_addr, ifname]
    return shlex.join(args)


def run_feature(cfg, feature, ipver):
    if not cfg.addr_v[ipver]:
        ipver = "4" if ipver == "6" else "6"
    cfg.require_ipver(ipver)
    dut_addr = mapped_address(cfg.addr_v[ipver])
    tester_addr = mapped_address(cfg.remote_addr_v[ipver])

    dut_cmd = feature_command(cfg.xdp_features, feature, dut_addr,
                              tester_addr, cfg.ifname)
    tester_cmd = feature_command(cfg.remote_xdp_features, feature, dut_addr,
                                 tester_addr, cfg.remote_ifname, tester=True)

    with bkg(dut_cmd, exit_wait=True, ksft_ready=True):
        result = cmd(tester_cmd, host=cfg.remote)

    output = ANSI_ESCAPE.sub("", result.stdout)
    match = RESULT.search(output)
    if not match:
        raise Exception(f"Unable to parse xdp_features output: {output}")

    detected = match.group(1) is None
    advertised = match.group(2) is None
    ksft_eq(detected, advertised,
            comment=f"{feature}: detected and advertised support")


def test_xdp_pass(cfg):
    run_feature(cfg, "XDP_PASS", "6")


def test_xdp_drop(cfg):
    run_feature(cfg, "XDP_DROP", "4")


def test_xdp_aborted(cfg):
    run_feature(cfg, "XDP_ABORTED", "6")


def test_xdp_tx(cfg):
    run_feature(cfg, "XDP_TX", "4")


def test_xdp_redirect(cfg):
    run_feature(cfg, "XDP_REDIRECT", "6")


def test_xdp_ndo_xmit(cfg):
    run_feature(cfg, "XDP_NDO_XMIT", "4")


def main():
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.xdp_features = (Path(__file__).parent / "xdp_features").resolve()
        cfg.remote_xdp_features = cfg.remote.deploy(
            cfg.xdp_features.as_posix()
        )

        ksft_run([
            test_xdp_pass,
            test_xdp_drop,
            test_xdp_aborted,
            test_xdp_tx,
            test_xdp_redirect,
            test_xdp_ndo_xmit,
        ], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
