#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Tests for ethtool loopback GET/SET with CMIS modules.

Works on any device that reports module loopback entries. On devices
without CMIS loopback support, tests are skipped.
"""

import errno

from lib.py import ksft_run, ksft_exit, ksft_eq
from lib.py import KsftSkipEx, KsftFailEx, ksft_disruptive
from lib.py import EthtoolFamily, NlError
from lib.py import NetDrvEnv, ip, defer

# Direction flags as YNL returns them (sets of flag name strings)
DIR_NONE = set()
DIR_LOCAL = {'local'}
DIR_REMOTE = {'remote'}


def _get_loopback(cfg):
    """GET loopback and return the list of entries (via DUMP)."""
    results = cfg.ethnl.loopback_get({
        'header': {'dev-index': cfg.ifindex}
    }, dump=True)
    entries = []
    for msg in results:
        if 'entry' in msg:
            entries.extend(msg['entry'])
    return entries


def _set_loopback(cfg, component, name, direction):
    """SET loopback for a single entry."""
    cfg.ethnl.loopback_set({
        'header': {'dev-index': cfg.ifindex},
        'entry': [{
            'component': component,
            'name': name,
            'direction': direction,
        }]
    })


def _require_module_entries(cfg):
    """Return module loopback entries, skip if none available."""
    entries = _get_loopback(cfg)
    mod_entries = [e for e in entries if e['component'] == 'module']
    if not mod_entries:
        raise KsftSkipEx("No module loopback entries")
    return mod_entries


@ksft_disruptive
def test_set_local(cfg):
    """SET a module entry to local loopback and verify via GET."""
    mod_entries = _require_module_entries(cfg)

    near = [e for e in mod_entries
            if 'local' in e['supported']]
    if not near:
        raise KsftSkipEx("No local capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = near[0]
    _set_loopback(cfg, 'module', target['name'], 'local')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries
               if e['name'] == target['name']
               and 'local' in e['supported']]
    ksft_eq(len(updated), 1)
    ksft_eq(updated[0]['direction'], DIR_LOCAL)


@ksft_disruptive
def test_set_remote(cfg):
    """SET a module entry to remote loopback and verify via GET."""
    mod_entries = _require_module_entries(cfg)

    far = [e for e in mod_entries
           if 'remote' in e['supported']]
    if not far:
        raise KsftSkipEx("No remote capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = far[0]
    _set_loopback(cfg, 'module', target['name'], 'remote')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries
               if e['name'] == target['name']
               and 'remote' in e['supported']]
    ksft_eq(len(updated), 1)
    ksft_eq(updated[0]['direction'], DIR_REMOTE)


@ksft_disruptive
def test_set_disable(cfg):
    """Enable then disable loopback and verify."""
    mod_entries = _require_module_entries(cfg)

    near = [e for e in mod_entries
            if 'local' in e['supported']]
    if not near:
        raise KsftSkipEx("No local capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = near[0]
    _set_loopback(cfg, 'module', target['name'], 'local')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    # Disable
    _set_loopback(cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries if e['name'] == target['name']]
    ksft_eq(updated[0]['direction'], DIR_NONE,
            "Direction should be off after disable")


@ksft_disruptive
def test_set_direction_switch(cfg):
    """Enable local, then switch to remote. The kernel must disable
    local before enabling remote (mutual exclusivity).
    """
    mod_entries = _require_module_entries(cfg)

    both = [e for e in mod_entries
            if 'local' in e['supported'] and 'remote' in e['supported']]
    if not both:
        raise KsftSkipEx("No entry with both local and remote support")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = both[0]
    _set_loopback(cfg, 'module', target['name'], 'local')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries if e['name'] == target['name']]
    ksft_eq(updated[0]['direction'], DIR_LOCAL)

    # Switch to remote
    _set_loopback(cfg, 'module', target['name'], 'remote')

    entries = _get_loopback(cfg)
    updated = [e for e in entries if e['name'] == target['name']]
    ksft_eq(updated[0]['direction'], DIR_REMOTE,
            "Should have switched to remote")


@ksft_disruptive
def test_set_idempotent(cfg):
    """Enable the same direction twice. Second call should not fail."""
    mod_entries = _require_module_entries(cfg)

    near = [e for e in mod_entries
            if 'local' in e['supported']]
    if not near:
        raise KsftSkipEx("No local capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = near[0]
    _set_loopback(cfg, 'module', target['name'], 'local')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    # Second enable of the same direction should succeed
    _set_loopback(cfg, 'module', target['name'], 'local')

    entries = _get_loopback(cfg)
    updated = [e for e in entries
               if e['name'] == target['name']
               and 'local' in e['supported']]
    ksft_eq(updated[0]['direction'], DIR_LOCAL,
            "Direction should still be local")


@ksft_disruptive
def test_set_while_up(cfg):
    """SET while interface is UP should fail."""
    mod_entries = _require_module_entries(cfg)

    target = mod_entries[0]
    direction = 'local'
    if direction not in target['supported']:
        direction = 'remote'

    try:
        _set_loopback(cfg, 'module', target['name'], direction)
        raise KsftFailEx("Should have rejected SET while interface is up")
    except NlError as e:
        ksft_eq(e.error, errno.EBUSY,
                "Expected EBUSY when interface is up")


def main() -> None:
    """Run loopback driver tests."""
    with NetDrvEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()

        ksft_run([
            test_set_local,
            test_set_remote,
            test_set_disable,
            test_set_direction_switch,
            test_set_idempotent,
            test_set_while_up,
        ], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
