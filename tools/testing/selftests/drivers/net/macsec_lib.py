# SPDX-License-Identifier: GPL-2.0

"""Shared helpers for MACsec offload tests."""

from lib.py import KsftSkipEx, ethtool, ip

MACSEC_KEY = "12345678901234567890123456789012"


def get_macsec_offload(dev):
    """Return macsec offload mode string from ip -d link show."""
    info = ip(f"-d link show dev {dev}", json=True)[0]
    return info.get("linkinfo", {}).get("info_data", {}).get("offload")


def require_macsec_offload(cfg):
    """SKIP if lower device doesn't support macsec-hw-offload."""
    feat = ethtool(f"-k {cfg.ifname}", json=True)[0]
    if not feat.get("macsec-hw-offload", {}).get("active"):
        raise KsftSkipEx("macsec-hw-offload not supported")
