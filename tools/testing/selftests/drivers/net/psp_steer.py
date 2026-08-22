#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Test suite for PSP virtualization cookie based Rx queue steering."""

import errno
import os
import socket
import time

from lib.py import defer
from lib.py import ksft_run, ksft_exit
from lib.py import ksft_eq, ksft_ge, ksft_in, ksft_lt, ksft_ne, ksft_raises
from lib.py import CmdExitFailure, KsftSkipEx
from lib.py import NetDrvEpEnv
from lib.py import NetdevFamily, NlError, PSPFamily
from lib.py import ethtool

from psp_lib import close_conn, init_psp_dev, make_clr_conn, make_psp_conn, \
    psp_txrx, remote_dev_steer, req_echo, spi_xchg
from psp_lib import responder as psp_responder

# Not exposed by the socket module
_SO_INCOMING_NAPI_ID = 56


# Mirrors the helpers of the same name in hw/rss_ctx.py, which lives in a
# directory this test cannot import from.
def ethtool_create(cfg, act, opts):
    output = ethtool(f"{act} {cfg.ifname} {opts}").stdout
    # "New RSS context is 1" / "Added rule with ID 7", we want the integer
    return int(output.split()[-1])


def require_ntuple(cfg):
    features = ethtool(f"-k {cfg.ifname}", json=True)[0]
    if not features["ntuple-filters"]["active"]:
        if features["ntuple-filters"]["fixed"]:
            raise KsftSkipEx("Device does not support ntuple-filters")
        ethtool(f"-K {cfg.ifname} ntuple-filters on")
        defer(ethtool, f"-K {cfg.ifname} ntuple-filters off")

_VC_TX = 1 << 0
_VC_RX = 1 << 1
_VC_BOTH = _VC_TX | _VC_RX
_VC_SIZE = 8
_IDLE_TIME = 0.5
_FALLBACK_QUEUE = 3


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


def _rx_queue(cfg, s):
    """Rx queue the socket's last packet arrived on"""
    napi_id = s.getsockopt(socket.SOL_SOCKET, _SO_INCOMING_NAPI_ID)
    ksft_ne(napi_id, 0, comment="socket saw no traffic?")
    ksft_in(napi_id, cfg.napi2queue, comment="unknown NAPI id")
    return cfg.napi2queue[napi_id]


def _settled_rx_queue(cfg, s, sent):
    """Run traffic until the peer picked our request up, report the queue

    The first exchange carries our request to the peer, the second comes
    back already steered.
    """
    sent = psp_txrx(cfg, s, 1, sent)
    sent = psp_txrx(cfg, s, 1, sent)

    return _rx_queue(cfg, s), sent


def _rss_pin(cfg, qid, context=None):
    """Point an RSS indirection table at a single queue"""
    ctx = f"context {context} " if context is not None else ""
    weights = " ".join("1" if i == qid else "0"
                       for i in range(cfg.rx_queue_cnt))
    ethtool(f"-X {cfg.ifname} {ctx}weight {weights}")


def _rss_steers(cfg):
    """Does the Rx queue actually follow the RSS table?

    Steering can only be compared against RSS on a device where RSS has a
    say in the first place - netdevsim, for one, ignores the table. Leaves
    the table as it found it.
    """
    probe = cfg.rx_queue_cnt - 1

    try:
        _rss_pin(cfg, probe)
    except CmdExitFailure:
        return False
    defer(ethtool, f"-X {cfg.ifname} default")

    with make_clr_conn(cfg) as s:
        psp_txrx(cfg, s, 1)
        landed = _rx_queue(cfg, s)
        close_conn(cfg, s)

    ethtool(f"-X {cfg.ifname} default")
    return landed == probe


def _require_rss_steering(cfg):
    """Skip unless the Rx queue follows the RSS table"""
    if not _rss_steers(cfg):
        raise KsftSkipEx("Rx queue does not follow the RSS table")


def _set_queue_cnt(cfg, cnt):
    """Reconfigure the device, and re-read the NAPI ids it hands out"""
    ethtool(f"-L {cfg.ifname} combined {cnt}")
    _queue_info(cfg)


def _ntuple_l3_rule(cfg, target):
    """Steer this host's traffic with an L3 only rule, and clean it up

    L3 only on purpose: whether the classifier sees the inner TCP ports
    of a PSP packet or just the outer UDP encapsulation is up to the
    device, the addresses are there either way.
    """
    flow = (f"flow-type ip{cfg.addr_ipver} "
            f"src-ip {cfg.remote_addr} dst-ip {cfg.addr} {target}")
    rule = ethtool_create(cfg, "-N", flow)
    defer(ethtool, f"-N {cfg.ifname} delete {rule}")
    return rule


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


def data_steer_no_grant(cfg):
    """ We ask and nobody grants: nothing is steered """
    _require_steer(cfg)
    _require_queues(cfg, 3)
    _require_rss_steering(cfg)
    _enable_steer(cfg, local=_VC_RX, remote=0)

    # RSS says 2, we would be asking for 1 if anyone were listening
    _rss_pin(cfg, 2)
    defer(_force_tx_queue, cfg, -1)
    _force_tx_queue(cfg, 1)

    with _psp_conn(cfg) as s:
        qid, _ = _settled_rx_queue(cfg, s, 0)
        ksft_eq(qid, 2, comment="steered without the peer granting anything")
        close_conn(cfg, s)


def data_steer_beats_rss(cfg):
    """ Steering has to win over the RSS table """
    _require_steer(cfg)
    _require_queues(cfg, 3)
    _enable_steer(cfg)
    _require_rss_steering(cfg)

    # RSS says 2, steering is going to ask for 1
    _rss_pin(cfg, 2)
    defer(_force_tx_queue, cfg, -1)
    _force_tx_queue(cfg, 1)

    with _psp_conn(cfg) as s:
        qid, _ = _settled_rx_queue(cfg, s, 0)
        ksft_eq(qid, 1)
        close_conn(cfg, s)


def data_steer_beats_rss_ctx(cfg):
    """ Steering has to win over an additional RSS context as well """
    _require_steer(cfg)
    _require_queues(cfg, 3)
    _enable_steer(cfg)
    _require_rss_steering(cfg)
    require_ntuple(cfg)

    # Three distinct answers: default RSS says 0, the context says 2,
    # and steering is going to ask for 1.
    _rss_pin(cfg, 0)
    ctx = ethtool_create(cfg, "-X", "context new")
    defer(ethtool, f"-X {cfg.ifname} context {ctx} delete")
    _rss_pin(cfg, 2, context=ctx)
    _ntuple_l3_rule(cfg, f"context {ctx}")

    # Without a cookie the context has to be the one deciding, otherwise
    # the check below would pass without steering doing anything.
    with make_clr_conn(cfg) as s:
        psp_txrx(cfg, s, 1)
        ksft_eq(_rx_queue(cfg, s), 2, comment="RSS context not in use")
        close_conn(cfg, s)

    defer(_force_tx_queue, cfg, -1)
    _force_tx_queue(cfg, 1)

    with _psp_conn(cfg) as s:
        qid, _ = _settled_rx_queue(cfg, s, 0)
        ksft_eq(qid, 1, comment="steering did not escape the RSS context")
        close_conn(cfg, s)


def data_ntuple_beats_steer(cfg):
    """ An ntuple rule naming a queue outranks steering """
    _require_steer(cfg)
    _require_queues(cfg, 3)
    _enable_steer(cfg)
    _require_rss_steering(cfg)
    require_ntuple(cfg)

    # RSS says 0, the rule says 2, steering is going to ask for 1
    _rss_pin(cfg, 0)
    _ntuple_l3_rule(cfg, "action 2")

    defer(_force_tx_queue, cfg, -1)
    _force_tx_queue(cfg, 1)

    with _psp_conn(cfg) as s:
        qid, _ = _settled_rx_queue(cfg, s, 0)
        ksft_eq(qid, 2, comment="steering overrode an explicit ntuple rule")
        close_conn(cfg, s)


def data_steer_stale_queue(cfg):
    """ A cookie naming a queue which went away has to fall back to RSS """
    _require_steer(cfg)
    _require_queues(cfg, 8)
    _enable_steer(cfg)

    nq = cfg.rx_queue_cnt
    rss = _rss_steers(cfg)
    if rss:
        # Point RSS at a queue of our choosing so that "it fell back to
        # RSS" is a statement we can actually check. Deliberately not
        # queue 0: plenty of devices use that as a default or error queue,
        # and landing there would prove nothing - it would also be a
        # thundering herd waiting to happen if every stale flow went there.
        _rss_pin(cfg, _FALLBACK_QUEUE)

    defer(_set_queue_cnt, cfg, nq)
    defer(_force_tx_queue, cfg, -1)
    _force_tx_queue(cfg, nq - 1)

    with _psp_conn(cfg) as s:
        qid, _ = _settled_rx_queue(cfg, s, 0)
        ksft_eq(qid, nq - 1)

        # Go properly idle first. A delayed ACK landing after the
        # reconfiguration would carry a fresh request and teach the peer a
        # live queue, and we would end up measuring nothing.
        time.sleep(_IDLE_TIME)

        # Take the queue away without telling the peer, which goes on
        # asking for it in every cookie it sends.
        _set_queue_cnt(cfg, nq - 1)

        # One packet, so that our ACK cannot teach the peer a new queue
        # before we get to look at where this one landed.
        req_echo(cfg, s)
        qid = _rx_queue(cfg, s)
        close_conn(cfg, s)

    ksft_lt(qid, nq - 1, comment="delivered to a queue which no longer exists")
    if rss:
        ksft_eq(qid, _FALLBACK_QUEUE, comment="stale request did not fall back to RSS")


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

    with NetDrvEpEnv(__file__, queue_count=8) as cfg:
        cfg.pspnl = PSPFamily()
        _queue_info(cfg)

        with psp_responder(cfg):
            ksft_run(globs=globals(),
                     case_pfx={"dev_", "assoc_", "data_"},
                     args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
