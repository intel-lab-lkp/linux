#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""MACsec offload API and ethtool feature tests."""

from lib.py import ksft_run, ksft_exit, ksft_eq, ksft_raises
from lib.py import CmdExitFailure
from lib.py import NetDrvEnv
from lib.py import ip, defer
from macsec_lib import require_macsec_offload, get_macsec_offload


def test_offload_api(cfg) -> None:
    """MACsec offload API: create SecY, add SA/rx, toggle offload."""

    require_macsec_offload(cfg)
    # Create 3 SecY with offload
    ip(f"link add link {cfg.ifname} macsec0 type macsec "
       f"port 4 encrypt on offload mac")
    defer(ip, f"link del macsec0")

    ip(f"link add link {cfg.ifname} macsec1 type macsec "
       f"address aa:bb:cc:dd:ee:ff port 5 encrypt on offload mac")
    defer(ip, f"link del macsec1")

    ip(f"link add link {cfg.ifname} macsec2 type macsec "
       f"sci abbacdde01020304 encrypt on offload mac")
    defer(ip, f"link del macsec2")

    # nsim-only: 4th SecY should fail (max 3)
    if cfg._ns is not None:
        with ksft_raises(CmdExitFailure):
            ip(f"link add link {cfg.ifname} macsec3 "
               f"type macsec port 8 encrypt on offload mac")

    # Add TX SA
    ip(f"macsec add macsec0 tx sa 0 pn 1024 on "
       f"key 01 12345678901234567890123456789012")

    # Add RX SC + SA
    ip(f'macsec add macsec0 rx port 1234 address 1c:ed:de:ad:be:ef')
    ip(f'macsec add macsec0 rx port 1234 address 1c:ed:de:ad:be:ef '
       f"sa 0 pn 1 on key 00 0123456789abcdef0123456789abcdef")

    # nsim-only: 2nd RX SC should fail (max 1)
    if cfg._ns is not None:
        with ksft_raises(CmdExitFailure):
            ip(f'macsec add macsec0 rx port 1235 address 1c:ed:de:ad:be:ef')

    # Can't disable offload when SAs are configured
    with ksft_raises(CmdExitFailure):
        ip(f"link set macsec0 type macsec offload off")
    with ksft_raises(CmdExitFailure):
        ip(f"macsec offload macsec0 off")

    # Toggle offload via rtnetlink on SA-free device
    ip(f"link set macsec2 type macsec offload off")
    ip(f"link set macsec2 type macsec encrypt on offload mac")

    # Toggle offload via genetlink
    ip(f"macsec offload macsec2 off")
    ip(f"macsec offload macsec2 mac")


def test_offload_state(cfg) -> None:
    """Offload state reflects configuration changes."""

    require_macsec_offload(cfg)
    # Create with offload on
    ip(f"link add link {cfg.ifname} macsec0 type macsec "
       f"encrypt on offload mac")
    ksft_eq(get_macsec_offload("macsec0"), "mac",
            "created with offload: should be mac")

    ip(f"link set macsec0 type macsec offload off")
    ksft_eq(get_macsec_offload("macsec0"), "off",
            "offload disabled: should be off")

    ip(f"link set macsec0 type macsec encrypt on offload mac")
    ksft_eq(get_macsec_offload("macsec0"), "mac",
            "offload re-enabled: should be mac")

    # Delete and recreate without offload
    ip(f"link del macsec0")
    ip(f"link add link {cfg.ifname} macsec0 type macsec")
    defer(ip, f"link del macsec0")
    ksft_eq(get_macsec_offload("macsec0"), "off",
            "created without offload: should be off")

    ip(f"link set macsec0 type macsec encrypt on offload mac")
    ksft_eq(get_macsec_offload("macsec0"), "mac",
            "offload enabled after create: should be mac")


def main() -> None:
    with NetDrvEnv(__file__) as cfg:
        ksft_run([test_offload_api,
                  test_offload_state], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
