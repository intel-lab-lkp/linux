#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Netdevsim-specific tests for MAC loopback via ethtool_ops.

Verifies that MAC loopback entries appear in dumps, that SET
operations update state correctly (both via GET and debugfs).
"""

import errno
import os

from lib.py import ksft_run, ksft_exit, ksft_eq
from lib.py import KsftFailEx, ksft_disruptive
from lib.py import EthtoolFamily, NlError
from lib.py import NetDrvEnv, ip, defer

# Direction flags as YNL returns them
DIR_NONE = set()
DIR_LOCAL = {'local'}
DIR_REMOTE = {'remote'}


def _nsim_dfs_path(cfg):
    return cfg._ns.nsims[0].dfs_dir  # pylint: disable=protected-access


def _dfs_read_u32(cfg, path):
    with open(os.path.join(_nsim_dfs_path(cfg), path),
              encoding="utf-8") as f:
        return int(f.read().strip())


def _dfs_write_u32(cfg, path, val):
    with open(os.path.join(_nsim_dfs_path(cfg), path), "w",
              encoding="utf-8") as f:
        f.write(str(val))


def _get_loopback(cfg):
    results = cfg.ethnl.loopback_get({
        'header': {'dev-index': cfg.ifindex}
    }, dump=True)
    entries = []
    for msg in results:
        if 'entry' in msg:
            entries.extend(msg['entry'])
    return entries


def _set_loopback(cfg, component, name, direction):
    cfg.ethnl.loopback_set({
        'header': {'dev-index': cfg.ifindex},
        'entry': [{
            'component': component,
            'name': name,
            'direction': direction,
        }]
    })


def test_get_mac_entry(cfg):
    """GET should return the MAC loopback entry with correct attributes."""
    entries = _get_loopback(cfg)
    mac_entries = [e for e in entries if e['component'] == 'mac']

    ksft_eq(len(mac_entries), 1, "Expected 1 MAC loopback entry")
    ksft_eq(mac_entries[0]['name'], 'mac')
    ksft_eq(mac_entries[0]['supported'], DIR_LOCAL | DIR_REMOTE)
    ksft_eq(mac_entries[0]['direction'], DIR_NONE)


@ksft_disruptive
def test_set_mac_local(cfg):
    """SET MAC local loopback and verify via GET and debugfs."""
    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    _set_loopback(cfg, 'mac', 'mac', 'local')
    defer(_set_loopback, cfg, 'mac', 'mac', 0)

    entries = _get_loopback(cfg)
    mac = [e for e in entries if e['component'] == 'mac']
    ksft_eq(mac[0]['direction'], DIR_LOCAL)

    dfs_dir = _dfs_read_u32(cfg, "ethtool/mac_lb/direction")
    ksft_eq(dfs_dir, 1, "debugfs direction should be 1 (LOCAL)")


@ksft_disruptive
def test_set_mac_disable(cfg):
    """Enable then disable MAC loopback."""
    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    _set_loopback(cfg, 'mac', 'mac', 'local')
    defer(_set_loopback, cfg, 'mac', 'mac', 0)

    _set_loopback(cfg, 'mac', 'mac', 0)

    entries = _get_loopback(cfg)
    mac = [e for e in entries if e['component'] == 'mac']
    ksft_eq(mac[0]['direction'], DIR_NONE, "Direction should be off")

    dfs_dir = _dfs_read_u32(cfg, "ethtool/mac_lb/direction")
    ksft_eq(dfs_dir, 0, "debugfs direction should be 0")


@ksft_disruptive
def test_set_mac_unknown_name(cfg):
    """SET with unknown name should fail with EOPNOTSUPP."""
    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    try:
        _set_loopback(cfg, 'mac', 'bogus', 'local')
        raise KsftFailEx("Should have rejected unknown name")
    except NlError as e:
        ksft_eq(e.error, errno.EOPNOTSUPP,
                "Expected EOPNOTSUPP for unknown name")


def main() -> None:
    """Run netdevsim loopback tests."""
    with NetDrvEnv(__file__) as cfg:
        cfg.ethnl = EthtoolFamily()

        ksft_run([
            test_get_mac_entry,
            test_set_mac_local,
            test_set_mac_disable,
            test_set_mac_unknown_name,
        ], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
