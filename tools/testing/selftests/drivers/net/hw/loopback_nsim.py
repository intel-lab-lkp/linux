#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Netdevsim-specific tests for ethtool loopback.

Verifies that MAC loopback entries appear in dumps, that SET
operations update state correctly (both via GET and debugfs).

Seeds the CMIS EEPROM via debugfs and verifies register-level
behavior that can only be checked with controlled EEPROM contents.
"""

import errno
import os

from lib.py import ksft_run, ksft_exit, ksft_eq
from lib.py import KsftFailEx, ksft_disruptive
from lib.py import EthtoolFamily, NlError
from lib.py import NetDrvEnv, ip, defer

# CMIS register constants matching net/ethtool/cmis_loopback.c
SFF8024_ID_QSFP_DD = 0x18

# Page 01h, Byte 142: bit 5 = Page 13h supported
CMIS_DIAG_PAGE13_BIT = 1 << 5

# Page 13h, Byte 128: loopback capability bits
CMIS_LB_CAP_MEDIA_OUTPUT = 1 << 0
CMIS_LB_CAP_MEDIA_INPUT = 1 << 1
CMIS_LB_CAP_HOST_OUTPUT = 1 << 2
CMIS_LB_CAP_HOST_INPUT = 1 << 3
CMIS_LB_CAP_ALL = (CMIS_LB_CAP_MEDIA_OUTPUT | CMIS_LB_CAP_MEDIA_INPUT |
                    CMIS_LB_CAP_HOST_OUTPUT | CMIS_LB_CAP_HOST_INPUT)

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


def _nsim_write_page_byte(cfg, page, offset, value):
    """Write a single byte to a netdevsim EEPROM page via debugfs."""
    if offset < 128:
        page_file = os.path.join(_nsim_dfs_path(cfg),
                                 "ethtool/module/pages/0")
        file_offset = offset
    else:
        page_file = os.path.join(_nsim_dfs_path(cfg),
                                 f"ethtool/module/pages/{page}")
        file_offset = offset - 128

    with open(page_file, "r+b") as f:
        f.seek(file_offset)
        f.write(bytes([value]))


def _nsim_read_page_byte(cfg, page, offset):
    """Read a single byte from a netdevsim EEPROM page via debugfs."""
    if offset < 128:
        page_file = os.path.join(_nsim_dfs_path(cfg),
                                 "ethtool/module/pages/0")
        file_offset = offset
    else:
        page_file = os.path.join(_nsim_dfs_path(cfg),
                                 f"ethtool/module/pages/{page}")
        file_offset = offset - 128

    with open(page_file, "rb") as f:
        f.seek(file_offset)
        return f.read(1)[0]


def _nsim_seed_cmis(cfg, caps=CMIS_LB_CAP_ALL):
    """Seed the netdevsim EEPROM with CMIS module identity and
    loopback capabilities.
    """
    _nsim_write_page_byte(cfg, 0x00, 0, SFF8024_ID_QSFP_DD)
    _nsim_write_page_byte(cfg, 0x01, 0x8e, CMIS_DIAG_PAGE13_BIT)
    _nsim_write_page_byte(cfg, 0x13, 0x80, caps)


def _nsim_clear_cmis(cfg):
    """Clear CMIS identity bytes left by previous tests."""
    _nsim_write_page_byte(cfg, 0x00, 0, 0)
    _nsim_write_page_byte(cfg, 0x01, 0x8e, 0)
    _nsim_write_page_byte(cfg, 0x13, 0x80, 0)


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


def test_get_no_module(cfg):
    """GET on a device with no CMIS module returns no module entries."""
    _nsim_clear_cmis(cfg)

    entries = _get_loopback(cfg)
    mod_entries = [e for e in entries if e['component'] == 'module']
    ksft_eq(len(mod_entries), 0, "Expected no module entries without CMIS module")


def test_get_all_caps(cfg):
    """GET with all four CMIS loopback capabilities seeded."""
    _nsim_seed_cmis(cfg, CMIS_LB_CAP_ALL)

    entries = _get_loopback(cfg)
    mod_entries = [e for e in entries if e['component'] == 'module']

    # Expect 2 entries (one per name), each with both directions
    ksft_eq(len(mod_entries), 2, "Expected 2 module loopback entries")

    host = [e for e in mod_entries if e['name'] == 'cmis-host']
    media = [e for e in mod_entries if e['name'] == 'cmis-media']
    ksft_eq(len(host), 1, "Expected 1 cmis-host entry")
    ksft_eq(len(media), 1, "Expected 1 cmis-media entry")

    ksft_eq(host[0]['supported'], DIR_LOCAL | DIR_REMOTE)
    ksft_eq(media[0]['supported'], DIR_LOCAL | DIR_REMOTE)

    for e in mod_entries:
        ksft_eq(e['direction'], DIR_NONE,
                f"Expected direction=off for {e['name']}")


def test_get_partial_caps(cfg):
    """GET with only host-input capability advertised."""
    _nsim_seed_cmis(cfg, CMIS_LB_CAP_HOST_INPUT)

    entries = _get_loopback(cfg)
    mod_entries = [e for e in entries if e['component'] == 'module']
    ksft_eq(len(mod_entries), 1, "Expected 1 module loopback entry")
    ksft_eq(mod_entries[0]['name'], 'cmis-host')
    ksft_eq(mod_entries[0]['supported'], DIR_LOCAL)


@ksft_disruptive
def test_set_verify_eeprom(cfg):
    """SET local loopback and verify the EEPROM control byte directly."""
    _nsim_seed_cmis(cfg, CMIS_LB_CAP_ALL)

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    _set_loopback(cfg, 'module', 'cmis-host', 'local')
    defer(_set_loopback, cfg, 'module', 'cmis-host', 0)

    # Host Side Input = Page 13h, Byte 183
    val = _nsim_read_page_byte(cfg, 0x13, 183)
    ksft_eq(val, 0xff, "Host Side Input control byte should be 0xff")

    # Disable and verify
    _set_loopback(cfg, 'module', 'cmis-host', 0)
    val = _nsim_read_page_byte(cfg, 0x13, 183)
    ksft_eq(val, 0x00, "Host Side Input should be 0x00 after disable")


@ksft_disruptive
def test_set_direction_switch_eeprom(cfg):
    """Switch directions and verify both EEPROM bytes."""
    _nsim_seed_cmis(cfg, CMIS_LB_CAP_ALL)

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    _set_loopback(cfg, 'module', 'cmis-host', 'local')
    defer(_set_loopback, cfg, 'module', 'cmis-host', 0)

    # Switch to remote
    _set_loopback(cfg, 'module', 'cmis-host', 'remote')

    # Local (Host Input, Byte 183) should be disabled
    val = _nsim_read_page_byte(cfg, 0x13, 183)
    ksft_eq(val, 0x00, "Local should be disabled after switch")
    # Remote (Host Output, Byte 182) should be enabled
    val = _nsim_read_page_byte(cfg, 0x13, 182)
    ksft_eq(val, 0xff, "Remote should be enabled after switch")


@ksft_disruptive
def test_set_unsupported_direction(cfg):
    """SET with unsupported direction should fail."""
    _nsim_seed_cmis(cfg, CMIS_LB_CAP_HOST_INPUT)  # only local

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    try:
        _set_loopback(cfg, 'module', 'cmis-host', 'remote')
        raise KsftFailEx("Should have rejected unsupported direction")
    except NlError as e:
        ksft_eq(e.error, errno.EOPNOTSUPP,
                "Expected EOPNOTSUPP for unsupported direction")


@ksft_disruptive
def test_set_no_cmis(cfg):
    """SET on a device without CMIS loopback support should fail."""
    _nsim_clear_cmis(cfg)

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    try:
        _set_loopback(cfg, 'module', 'cmis-host', 'local')
        raise KsftFailEx("Should have rejected SET without CMIS support")
    except NlError as e:
        ksft_eq(e.error, errno.EOPNOTSUPP,
                "Expected EOPNOTSUPP without CMIS support")


def test_combined_dump(cfg):
    """Dump should return both MAC and MODULE entries."""
    _nsim_seed_cmis(cfg, CMIS_LB_CAP_ALL)
    defer(_nsim_clear_cmis, cfg)

    entries = _get_loopback(cfg)
    mac_entries = [e for e in entries if e['component'] == 'mac']
    mod_entries = [e for e in entries if e['component'] == 'module']

    ksft_eq(len(mac_entries), 1, "Expected 1 MAC entry")
    ksft_eq(len(mod_entries), 2, "Expected 2 MODULE entries")
    ksft_eq(mac_entries[0]['name'], 'mac')


def main() -> None:
    """Run netdevsim loopback tests."""
    with NetDrvEnv(__file__) as cfg:
        cfg.ethnl = EthtoolFamily()

        ksft_run([
            test_get_mac_entry,
            test_set_mac_local,
            test_set_mac_disable,
            test_set_mac_unknown_name,
            test_get_no_module,
            test_get_all_caps,
            test_get_partial_caps,
            test_set_verify_eeprom,
            test_set_direction_switch_eeprom,
            test_set_unsupported_direction,
            test_set_no_cmis,
            test_combined_dump,
        ], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
