#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import errno
import os
from lib.py import ksft_run, ksft_exit, ksft_eq, ksft_raises, KsftSkipEx
from lib.py import CmdExitFailure, EthtoolFamily, NlError
from lib.py import NetDrvEnv
from lib.py import defer, ip


def _get_hds_mode(cfg, netnl) -> str:
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'tcp-data-split' not in rings:
        raise KsftSkipEx('tcp-data-split not supported by device')
    return rings['tcp-data-split']


def get_hds(cfg, netnl) -> None:
    _get_hds_mode(cfg, netnl)


def get_hds_thresh(cfg, netnl) -> None:
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'hds-thresh' not in rings:
        raise KsftSkipEx('hds-thresh not supported by device')

def set_hds_enable(cfg, netnl) -> None:
    try:
        netnl.rings_set({'header': {'dev-index': cfg.ifindex}, 'tcp-data-split': 'enabled'})
    except NlError as e:
        if e.error == errno.EINVAL:
            raise KsftSkipEx("disabling of HDS not supported by the device")
        elif e.error == errno.EOPNOTSUPP:
            raise KsftSkipEx("ring-set not supported by the device")
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'tcp-data-split' not in rings:
        raise KsftSkipEx('tcp-data-split not supported by device')

    ksft_eq('enabled', rings['tcp-data-split'])

def set_hds_disable(cfg, netnl) -> None:
    try:
        netnl.rings_set({'header': {'dev-index': cfg.ifindex}, 'tcp-data-split': 'disabled'})
    except NlError as e:
        if e.error == errno.EINVAL:
            raise KsftSkipEx("disabling of HDS not supported by the device")
        elif e.error == errno.EOPNOTSUPP:
            raise KsftSkipEx("ring-set not supported by the device")
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'tcp-data-split' not in rings:
        raise KsftSkipEx('tcp-data-split not supported by device')

    ksft_eq('disabled', rings['tcp-data-split'])

def set_hds_thresh_zero(cfg, netnl) -> None:
    try:
        netnl.rings_set({'header': {'dev-index': cfg.ifindex}, 'hds-thresh': 0})
    except NlError as e:
        if e.error == errno.EINVAL:
            raise KsftSkipEx("hds-thresh-set not supported by the device")
        elif e.error == errno.EOPNOTSUPP:
            raise KsftSkipEx("ring-set not supported by the device")
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'hds-thresh' not in rings:
        raise KsftSkipEx('hds-thresh not supported by device')

    ksft_eq(0, rings['hds-thresh'])

def set_hds_thresh_max(cfg, netnl) -> None:
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'hds-thresh' not in rings:
        raise KsftSkipEx('hds-thresh not supported by device')
    try:
        netnl.rings_set({'header': {'dev-index': cfg.ifindex}, 'hds-thresh': rings['hds-thresh-max']})
    except NlError as e:
        if e.error == errno.EINVAL:
            raise KsftSkipEx("hds-thresh-set not supported by the device")
        elif e.error == errno.EOPNOTSUPP:
            raise KsftSkipEx("ring-set not supported by the device")
    rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    ksft_eq(rings['hds-thresh'], rings['hds-thresh-max'])

def set_hds_thresh_gt(cfg, netnl) -> None:
    try:
        rings = netnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        raise KsftSkipEx('ring-get not supported by device')
    if 'hds-thresh' not in rings:
        raise KsftSkipEx('hds-thresh not supported by device')
    if 'hds-thresh-max' not in rings:
        raise KsftSkipEx('hds-thresh-max not defined by device')
    hds_gt = rings['hds-thresh-max'] + 1
    with ksft_raises(NlError) as e:
        netnl.rings_set({'header': {'dev-index': cfg.ifindex}, 'hds-thresh': hds_gt})
    ksft_eq(e.exception.nl_msg.error, -errno.EINVAL)


def set_xdp(cfg, netnl) -> None:
    """
    Enable single-buffer XDP on the device.
    When HDS is in "auto" / UNKNOWN mode, XDP installation should work.
    """
    mode = _get_hds_mode(cfg, netnl)
    if mode == 'enabled':
        netnl.rings_set({'header': {'dev-index': cfg.ifindex},
                         'tcp-data-split': 'unknown'})

    test_dir = os.path.dirname(os.path.realpath(__file__))
    prog = test_dir + "/../../net/lib/xdp_dummy.bpf.o"
    ip(f"link set dev %s xdp obj %s sec xdp" %
        (cfg.ifname, prog))
    ip(f"link set dev %s xdp off" % cfg.ifname)


def set_xdp_enabled(cfg, netnl) -> None:
    """
    Enable single-buffer XDP on the device.
    When HDS is in "enabled" mode, XDP installation should not work.
    """
    _get_hds_mode(cfg, netnl)
    netnl.rings_set({'header': {'dev-index': cfg.ifindex},
                     'tcp-data-split': 'enabled'})

    defer(netnl.rings_set, {'header': {'dev-index': cfg.ifindex},
                            'tcp-data-split': 'unknown'})

    test_dir = os.path.dirname(os.path.realpath(__file__))
    prog = test_dir + "/../../net/lib/xdp_dummy.bpf.o"
    with ksft_raises(CmdExitFailure) as e:
        ip(f"link set dev %s xdp obj %s sec xdp" %
            (cfg.ifname, prog))
        ip(f"link set dev %s xdp off" % cfg.ifname)


def main() -> None:
    with NetDrvEnv(__file__, queue_count=3) as cfg:
        ksft_run([get_hds,
                  get_hds_thresh,
                  set_hds_disable,
                  set_hds_enable,
                  set_hds_thresh_zero,
                  set_hds_thresh_max,
                  set_hds_thresh_gt,
                  set_xdp,
                  set_xdp_enabled],
                 args=(cfg, EthtoolFamily()))
    ksft_exit()

if __name__ == "__main__":
    main()
