#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
VLAN tests.

Validates that ping traffic is sent and received correctly over 802.1q
and 802.1ad VLAN interfaces, with hardware VLAN stripping enabled and
disabled.

Test cases:
  - 8021q_hw:  Traffic over a single 802.1q VLAN, HW stripping on
  - 8021q_sw:  Traffic over a single 802.1q VLAN, HW stripping off
  - 8021ad_hw: Traffic over a single 802.1ad VLAN, HW stripping on
  - 8021ad_sw: Traffic over a single 802.1ad VLAN, HW stripping off
  - qinq_hw:   Traffic over an 802.1q VLAN stacked on an 802.1ad VLAN,
               HW stripping on
  - qinq_sw:   Traffic over an 802.1q VLAN stacked on an 802.1ad VLAN,
               HW stripping off
"""

import os
from lib.py import ksft_run, ksft_exit
from lib.py import NetDrvEpEnv
from lib.py import cmd, defer, ethtool, ip, set_ethtool_feat
from lib.py import ksft_variants, KsftNamedVariant

OUTER_DEV = f"vlout{os.getpid()}"
INNER_DEV = f"vlin{os.getpid()}"

OUTER_VID = 100
INNER_VID = 200

LOCAL_IP = "198.51.100.1"
REMOTE_IP = "198.51.100.2"


def _vlan_add(base, name, proto, vid, host=None):
    """Create a VLAN device on top of base and bring it up."""

    ip(f"link add link {base} name {name} type vlan proto {proto} id {vid}",
       host=host)
    defer(ip, f"link del {name}", host=host)
    ip(f"link set {name} up", host=host)


def _vlan_setup(base, addr, outer_proto, inner_proto, host=None):
    """Create VLAN interfaces on base and set an IP on the innermost one."""

    _vlan_add(base, OUTER_DEV, outer_proto, OUTER_VID, host=host)
    if inner_proto:
        _vlan_add(OUTER_DEV, INNER_DEV, inner_proto, INNER_VID, host=host)

    dev = INNER_DEV if inner_proto else OUTER_DEV
    ip(f"addr add {addr}/24 dev {dev}", host=host)


def _setup(cfg, outer_proto, inner_proto, hw_strip):
    """Configure VLAN stripping and create the VLAN interfaces."""

    feat = ethtool(f"-k {cfg.ifname}", json=True)[0]
    set_ethtool_feat(cfg.ifname, feat, {"rx-vlan-offload": hw_strip})

    _vlan_setup(cfg.ifname, LOCAL_IP, outer_proto, inner_proto)
    _vlan_setup(cfg.remote_ifname, REMOTE_IP, outer_proto, inner_proto,
                host=cfg.remote)


def _vlan_variants():
    """Generator that yields the VLAN protocols and the stripping mode."""

    yield KsftNamedVariant("8021q_hw", "802.1q", None, True)
    yield KsftNamedVariant("8021q_sw", "802.1q", None, False)
    yield KsftNamedVariant("8021ad_hw", "802.1ad", None, True)
    yield KsftNamedVariant("8021ad_sw", "802.1ad", None, False)
    yield KsftNamedVariant("qinq_hw", "802.1ad", "802.1q", True)
    yield KsftNamedVariant("qinq_sw", "802.1ad", "802.1q", False)


@ksft_variants(_vlan_variants())
def test(cfg, outer_proto, inner_proto, hw_strip):
    """Run a single VLAN test"""

    cfg.require_ipver("4")

    _setup(cfg, outer_proto, inner_proto, hw_strip)

    cmd(f"ping -c 1 -W 5 {REMOTE_IP}")


def main() -> None:
    """ Ksft boiler plate main """

    with NetDrvEpEnv(__file__) as cfg:
        ksft_run(cases=[test], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
