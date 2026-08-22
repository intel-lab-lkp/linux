#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Test suite for PSP virtualization cookie based Rx queue steering."""

import errno
import os
import socket

from lib.py import defer
from lib.py import ksft_run, ksft_exit
from lib.py import ksft_eq, ksft_ge, ksft_in, ksft_ne, ksft_raises
from lib.py import KsftSkipEx
from lib.py import NetDrvEpEnv
from lib.py import NetdevFamily, NlError, PSPFamily

from psp_lib import close_conn, init_psp_dev, make_psp_conn, psp_txrx, \
    remote_conn_steer, remote_dev_steer, spi_xchg
from psp_lib import responder as psp_responder

# Not exposed by the socket module
_SO_INCOMING_NAPI_ID = 56

_VC_TX = 1 << 0
_VC_RX = 1 << 1
_VC_BOTH = _VC_TX | _VC_RX
_VC_SIZE = 8


def _require_steer(cfg):
    """Skip unless the device can do VC steering"""
    init_psp_dev(cfg)

    if 'vc-steer-cap' not in cfg.psp_info:
        raise KsftSkipEx("Device does not support PSP VC steering")


def _require_queues(cfg, cnt):
    if cfg.rx_queue_cnt < cnt or cfg.tx_queue_cnt < cnt:
        raise KsftSkipEx(f"Test needs at least {cnt} Rx and Tx queues")


def _set_steer(cfg, mode):
    """Set vc-steer-ena locally for the duration of the test case"""
    dev = cfg.pspnl.dev_get({'id': cfg.psp_dev_id})
    prev = dev['vc-steer-ena']

    cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'vc-steer-ena': mode})
    defer(cfg.pspnl.dev_set, {'id': cfg.psp_dev_id, 'vc-steer-ena': prev})


def _set_remote_steer(cfg, mode):
    remote_dev_steer(cfg, mode)
    defer(remote_dev_steer, cfg, 0)


def _enable_steer(cfg, local=_VC_BOTH, remote=_VC_BOTH):
    """Turn steering on at both ends for the duration of the test case"""
    _set_steer(cfg, local)
    if remote is not None:
        _set_remote_steer(cfg, remote)


def _mss(s):
    return s.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG)


def _force_tx_queue(cfg, qid):
    """Point XPS at a single Tx queue, so we know the flow's Tx queue

    The queue we ask the peer to steer us to is taken from the Tx queue
    the stack picks for the flow, so pinning XPS is what makes the
    outcome predictable.
    """
    all_cpus = f'{(1 << os.cpu_count()) - 1:x}'
    for i in range(cfg.tx_queue_cnt):
        mask = all_cpus if i == qid else '0'
        with open(f'/sys/class/net/{cfg.ifname}/queues/tx-{i}/xps_cpus',
                  'w', encoding='ascii') as fp:
            fp.write(mask)


def _psp_conn(cfg):
    """Open a PSP connection, whatever the device is configured for"""
    s = make_psp_conn(cfg)

    rx = cfg.pspnl.rx_assoc({'version': 0, 'dev-id': cfg.psp_dev_id,
                             'sock-fd': s.fileno()})
    tx = spi_xchg(s, rx['rx-key'])
    cfg.pspnl.tx_assoc({'dev-id': cfg.psp_dev_id, 'version': 0,
                        'tx-key': tx, 'sock-fd': s.fileno()})
    return s


def _settled_rx_queue(cfg, s, sent):
    """Run traffic until the peer picked our request up, report the queue

    The first exchange carries our request to the peer, the second comes
    back already steered.
    """
    sent = psp_txrx(cfg, s, 1, sent)
    sent = psp_txrx(cfg, s, 1, sent)

    napi_id = s.getsockopt(socket.SOL_SOCKET, _SO_INCOMING_NAPI_ID)
    ksft_ne(napi_id, 0, comment="socket saw no traffic?")
    ksft_in(napi_id, cfg.napi2queue, comment="unknown NAPI id")
    return cfg.napi2queue[napi_id], sent


#
# Test cases
#

def dev_feature_toggle(cfg):
    """ Set each direction in turn, check it is reported back """
    _require_steer(cfg)

    dev = cfg.pspnl.dev_get({'id': cfg.psp_dev_id})
    defer(cfg.pspnl.dev_set, {'id': cfg.psp_dev_id,
                              'vc-steer-ena': dev['vc-steer-ena']})

    for mode in ({'tx'}, {'rx'}, {'tx', 'rx'}, set()):
        cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'vc-steer-ena': mode})
        dev = cfg.pspnl.dev_get({'id': cfg.psp_dev_id})
        ksft_eq(dev['vc-steer-ena'], mode)


def dev_feature_tx_needs_no_cap(cfg):
    """ Granting a peer's request must not depend on the device """
    init_psp_dev(cfg)

    dev = cfg.pspnl.dev_get({'id': cfg.psp_dev_id})
    defer(cfg.pspnl.dev_set, {'id': cfg.psp_dev_id,
                              'vc-steer-ena': dev['vc-steer-ena']})

    cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'vc-steer-ena': {'tx'}})
    dev = cfg.pspnl.dev_get({'id': cfg.psp_dev_id})
    ksft_eq(dev['vc-steer-ena'], {'tx'})


def dev_feature_rx_needs_cap(cfg):
    """ Steering our own Rx does need the device to play along """
    init_psp_dev(cfg)

    if 'vc-steer-cap' in cfg.psp_info:
        raise KsftSkipEx("Device can steer, nothing to reject")

    with ksft_raises(NlError) as cm:
        cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'vc-steer-ena': {'rx'}})
    ksft_eq(cm.exception.nl_msg.error, -errno.EOPNOTSUPP)


def dev_feature_bad_value(cfg):
    """ Only the two direction bits are valid """
    _require_steer(cfg)

    with ksft_raises(NlError) as cm:
        cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'vc-steer-ena': 0xdeadbeef})
    ksft_eq(cm.exception.nl_msg.error, -errno.EINVAL)


def data_mss_adjust(cfg):
    """ The cookie is 8B of extra header, the MSS has to account for it """
    _require_steer(cfg)

    _set_steer(cfg, 0)
    with _psp_conn(cfg) as s:
        plain = _mss(s)
        close_conn(cfg, s)

    # Either direction puts a cookie in every header we send
    for mode in (_VC_TX, _VC_RX, _VC_BOTH):
        _set_steer(cfg, mode)
        with _psp_conn(cfg) as s:
            ksft_eq(plain - _mss(s), _VC_SIZE, comment=f"mode {mode}")
            close_conn(cfg, s)


def data_mss_sampled_at_assoc(cfg):
    """ Turning steering on must not resize a live connection's header """
    _require_steer(cfg)

    _set_steer(cfg, 0)
    with _psp_conn(cfg) as s:
        before = _mss(s)
        _set_steer(cfg, _VC_BOTH)
        ksft_eq(_mss(s), before)
        close_conn(cfg, s)


def data_steer_follows_tx_queue(cfg):
    """ Traffic must land on the Rx queue paired with our Tx queue """
    _require_steer(cfg)
    _require_queues(cfg, 3)
    _enable_steer(cfg)

    defer(_force_tx_queue, cfg, -1)

    with _psp_conn(cfg) as s:
        sent = 0
        for qid in (1, 2):
            _force_tx_queue(cfg, qid)
            qid_seen, sent = _settled_rx_queue(cfg, s, sent)
            ksft_eq(qid_seen, qid)

        close_conn(cfg, s)


def data_steer_one_sided(cfg):
    """ We ask, the peer only grants: our Rx still gets steered """
    _require_steer(cfg)
    _require_queues(cfg, 2)
    _enable_steer(cfg, local=_VC_RX, remote=_VC_TX)

    defer(_force_tx_queue, cfg, -1)
    _force_tx_queue(cfg, 1)

    with _psp_conn(cfg) as s:
        qid, _ = _settled_rx_queue(cfg, s, 0)
        ksft_eq(qid, 1)
        close_conn(cfg, s)


def _queue_info(cfg):
    """Map NAPI ids to Rx queue ids, and count the queues"""
    netnl = NetdevFamily()
    queues = netnl.queue_get({'ifindex': cfg.ifindex}, dump=True)

    cfg.napi2queue = {}
    cfg.rx_queue_cnt = 0
    cfg.tx_queue_cnt = 0
    for q in queues:
        if q['type'] == 'rx':
            cfg.rx_queue_cnt += 1
            if 'napi-id' in q:
                cfg.napi2queue[q['napi-id']] = q['id']
        elif q['type'] == 'tx':
            cfg.tx_queue_cnt += 1


def main() -> None:
    """ Ksft boiler plate main """

    with NetDrvEpEnv(__file__, queue_count=4) as cfg:
        cfg.pspnl = PSPFamily()
        _queue_info(cfg)

        with psp_responder(cfg):
            ksft_run(globs=globals(),
                     case_pfx={"dev_", "assoc_", "data_"},
                     args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
