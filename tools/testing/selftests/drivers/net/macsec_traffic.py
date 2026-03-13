#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""MACsec VLAN propagation traffic tests."""

from lib.py import ksft_run, ksft_exit, ksft_pr, ksft_variants, KsftNamedVariant
from lib.py import CmdExitFailure
from lib.py import NetDrvEpEnv
from lib.py import cmd, ip, defer
from macsec_lib import require_macsec_offload

MACSEC_KEY = "12345678901234567890123456789012"
MACSEC_NAME = "macsec_t"
MACSEC_VLAN_VID = 10


def _get_mac(ifname, host=None):
    """Gets MAC address of an interface."""
    dev = ip(f"-d link show dev {ifname}", json=True, host=host)
    return dev[0]["address"]


def _setup_macsec_sa(cfg, name):
    """Adds matching TX/RX SAs on both ends."""
    local_mac = _get_mac(name)
    remote_mac = _get_mac(name, host=cfg.remote)

    ip(f"macsec add {name} tx sa 0 pn 1 on key 01 {MACSEC_KEY}")
    ip(f"macsec add {name} rx port 1 address {remote_mac}")
    ip(f"macsec add {name} rx port 1 address {remote_mac} "
       f"sa 0 pn 1 on key 02 {MACSEC_KEY}")

    ip(f"macsec add {name} tx sa 0 pn 1 on key 02 {MACSEC_KEY}",
       host=cfg.remote)
    ip(f"macsec add {name} rx port 1 address {local_mac}", host=cfg.remote)
    ip(f"macsec add {name} rx port 1 address {local_mac} "
       f"sa 0 pn 1 on key 01 {MACSEC_KEY}", host=cfg.remote)


def _setup_macsec_devs(cfg, name, offload):
    """Creates macsec devices on both ends."""
    offload_arg = "mac" if offload else "off"
    macsec_args = f"type macsec encrypt on offload {offload_arg}"

    ip(f"link add link {cfg.ifname} {name} {macsec_args}")
    defer(ip, f"link del {name}")
    ip(f"link add link {cfg.remote_ifname} {name} {macsec_args}",
       host=cfg.remote)
    defer(ip, f"link del {name}", host=cfg.remote)


def _set_offload(cfg, name, offload):
    """Sets offload on both macsec devices."""
    offload_arg = "mac" if offload else "off"

    ip(f"link set {name} type macsec encrypt on offload {offload_arg}")
    ip(f"link set {name} type macsec encrypt on offload {offload_arg}",
       host=cfg.remote)


def _setup_vlans(cfg, name, vid):
    """Adds VLANs on top of existing macsec devs."""
    vlan_name = f"{name}.{vid}"

    ip(f"link add link {name} {vlan_name} type vlan id {vid}")
    defer(ip, f"link del {vlan_name}")
    ip(f"link add link {name} {vlan_name} type vlan id {vid}", host=cfg.remote)
    defer(ip, f"link del {vlan_name}", host=cfg.remote)


def _setup_vlan_ips(cfg, name, vid):
    """Adds VLAN IPs."""
    local_ip = f"10.0.{vid}.1"
    remote_ip = f"10.0.{vid}.2"
    vlan_name = f"{name}.{vid}"

    ip(f"addr add {local_ip}/24 dev {vlan_name}")
    ip(f"addr add {remote_ip}/24 dev {vlan_name}", host=cfg.remote)
    ip(f"link set {name} up")
    ip(f"link set {name} up", host=cfg.remote)
    ip(f"link set {vlan_name} up")
    ip(f"link set {vlan_name} up", host=cfg.remote)

    return remote_ip


@ksft_variants([
    KsftNamedVariant("offloaded", True),
    KsftNamedVariant("software", False),
])
def test_vlan(cfg, offload) -> None:
    """Ping through VLAN-over-macsec."""

    require_macsec_offload(cfg)
    _setup_macsec_devs(cfg, MACSEC_NAME, offload=offload)
    _setup_macsec_sa(cfg, MACSEC_NAME)
    _setup_vlans(cfg, MACSEC_NAME, MACSEC_VLAN_VID)
    remote_ip = _setup_vlan_ips(cfg, MACSEC_NAME, MACSEC_VLAN_VID)
    cmd(f"ping -c 1 -W 5 {remote_ip}")


@ksft_variants([
    KsftNamedVariant("toggle_on", True),
    KsftNamedVariant("toggle_off", False),
])
def test_vlan_toggle(cfg, offload) -> None:
    """Toggle offload: VLAN filters propagate/remove correctly."""

    require_macsec_offload(cfg)
    _setup_macsec_devs(cfg, MACSEC_NAME, offload=offload)
    _setup_vlans(cfg, MACSEC_NAME, MACSEC_VLAN_VID)
    _set_offload(cfg, MACSEC_NAME, offload=not offload)
    remote_ip = _setup_vlan_ips(cfg, MACSEC_NAME, MACSEC_VLAN_VID)
    _setup_macsec_sa(cfg, MACSEC_NAME)
    cmd(f"ping -c 1 -W 5 {remote_ip}")


def main() -> None:
    with NetDrvEpEnv(__file__) as cfg:
        ksft_run([test_vlan,
                  test_vlan_toggle], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
