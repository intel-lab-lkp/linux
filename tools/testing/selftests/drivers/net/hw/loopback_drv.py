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
DIR_NEAR_END = {'near-end'}
DIR_FAR_END = {'far-end'}


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
def test_set_near_end(cfg):
    """SET a module entry to near-end and verify via GET."""
    mod_entries = _require_module_entries(cfg)

    near = [e for e in mod_entries
            if 'near-end' in e['supported']]
    if not near:
        raise KsftSkipEx("No near-end capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = near[0]
    _set_loopback(cfg, 'module', target['name'], 'near-end')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries
               if e['name'] == target['name']
               and 'near-end' in e['supported']]
    ksft_eq(len(updated), 1)
    ksft_eq(updated[0]['direction'], DIR_NEAR_END)


@ksft_disruptive
def test_set_far_end(cfg):
    """SET a module entry to far-end and verify via GET."""
    mod_entries = _require_module_entries(cfg)

    far = [e for e in mod_entries
           if 'far-end' in e['supported']]
    if not far:
        raise KsftSkipEx("No far-end capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = far[0]
    _set_loopback(cfg, 'module', target['name'], 'far-end')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries
               if e['name'] == target['name']
               and 'far-end' in e['supported']]
    ksft_eq(len(updated), 1)
    ksft_eq(updated[0]['direction'], DIR_FAR_END)


@ksft_disruptive
def test_set_disable(cfg):
    """Enable then disable loopback and verify."""
    mod_entries = _require_module_entries(cfg)

    near = [e for e in mod_entries
            if 'near-end' in e['supported']]
    if not near:
        raise KsftSkipEx("No near-end capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = near[0]
    _set_loopback(cfg, 'module', target['name'], 'near-end')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    # Disable
    _set_loopback(cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries if e['name'] == target['name']]
    ksft_eq(updated[0]['direction'], DIR_NONE,
            "Direction should be off after disable")


@ksft_disruptive
def test_set_direction_switch(cfg):
    """Enable near-end, then switch to far-end. The kernel must disable
    near-end before enabling far-end (mutual exclusivity).
    """
    mod_entries = _require_module_entries(cfg)

    both = [e for e in mod_entries
            if 'near-end' in e['supported'] and 'far-end' in e['supported']]
    if not both:
        raise KsftSkipEx("No entry with both near-end and far-end support")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = both[0]
    _set_loopback(cfg, 'module', target['name'], 'near-end')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    entries = _get_loopback(cfg)
    updated = [e for e in entries if e['name'] == target['name']]
    ksft_eq(updated[0]['direction'], DIR_NEAR_END)

    # Switch to far-end
    _set_loopback(cfg, 'module', target['name'], 'far-end')

    entries = _get_loopback(cfg)
    updated = [e for e in entries if e['name'] == target['name']]
    ksft_eq(updated[0]['direction'], DIR_FAR_END,
            "Should have switched to far-end")


@ksft_disruptive
def test_set_idempotent(cfg):
    """Enable the same direction twice. Second call should not fail."""
    mod_entries = _require_module_entries(cfg)

    near = [e for e in mod_entries
            if 'near-end' in e['supported']]
    if not near:
        raise KsftSkipEx("No near-end capable module entry")

    ip(f"link set dev {cfg.ifname} down")
    defer(ip, f"link set dev {cfg.ifname} up")

    target = near[0]
    _set_loopback(cfg, 'module', target['name'], 'near-end')
    defer(_set_loopback, cfg, 'module', target['name'], 0)

    # Second enable of the same direction should succeed
    _set_loopback(cfg, 'module', target['name'], 'near-end')

    entries = _get_loopback(cfg)
    updated = [e for e in entries
               if e['name'] == target['name']
               and 'near-end' in e['supported']]
    ksft_eq(updated[0]['direction'], DIR_NEAR_END,
            "Direction should still be near-end")


@ksft_disruptive
def test_set_while_up(cfg):
    """SET while interface is UP should fail."""
    mod_entries = _require_module_entries(cfg)

    target = mod_entries[0]
    direction = 'near-end'
    if direction not in target['supported']:
        direction = 'far-end'

    try:
        _set_loopback(cfg, 'module', target['name'], direction)
        raise KsftFailEx("Should have rejected SET while interface is up")
    except NlError as e:
        ksft_eq(e.error, errno.EBUSY,
                "Expected EBUSY when interface is up")


def main() -> None:
    with NetDrvEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()

        ksft_run([
            test_set_near_end,
            test_set_far_end,
            test_set_disable,
            test_set_direction_switch,
            test_set_idempotent,
            test_set_while_up,
        ], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
