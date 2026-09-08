# SPDX-License-Identifier: GPL-2.0

"""Shared helpers for the PSP selftests."""

import errno
import socket

from lib.py import defer
from lib.py import ksft_eq, ksft_raises
from lib.py import KsftSkipEx
from lib.py import NlError


def require_version(cfg, version):
    """Skip unless the device supports a PSP version.

    Version 0 is required by the spec, so it never skips.  Before
    skipping, check that the kernel rejects the unsupported version
    properly - this is the only coverage that path gets.
    """
    if not version:
        return

    name = cfg.pspnl.consts["version"].entries_by_val[version].name
    if name in cfg.psp_info['psp-versions-cap']:
        return

    with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as s:
        with ksft_raises(NlError) as cm:
            cfg.pspnl.rx_assoc({"version": version,
                                "dev-id": cfg.psp_dev_id,
                                "sock-fd": s.fileno()})
        ksft_eq(cm.exception.nl_msg.error, -errno.EOPNOTSUPP)
    raise KsftSkipEx("PSP version not supported", name)


def init_psp_dev(cfg, use_psp_ifindex=False):
    """Find the PSP device under test and enable all supported versions."""
    if not hasattr(cfg, 'psp_dev_id'):
        # Figure out which local device we are testing against
        # For NetDrvContEnv: use psp_ifindex instead of ifindex
        target_ifindex = cfg.psp_ifindex if use_psp_ifindex else cfg.ifindex
        for dev in cfg.pspnl.dev_get({}, dump=True):
            if dev['ifindex'] == target_ifindex:
                cfg.psp_info = dev
                cfg.psp_dev_id = cfg.psp_info['id']
                break
        else:
            raise KsftSkipEx("No PSP devices found")

    # Enable PSP if necessary
    cap = cfg.psp_info['psp-versions-cap']
    ena = cfg.psp_info['psp-versions-ena']
    if cap != ena:
        cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'psp-versions-ena': cap})
        defer(cfg.pspnl.dev_set, {'id': cfg.psp_dev_id,
                                  'psp-versions-ena': ena})
