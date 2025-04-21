#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import errno, time
from typing import Tuple
from lib.py import ksft_run, ksft_exit, KsftSkipEx, KsftFailEx
from lib.py import ksft_eq, ksft_ge, ksft_in, ksft_not_in
from lib.py import EthtoolFamily, NetdevFamily, NlError
from lib.py import NetDrvEpEnv, GenerateTraffic
from lib.py import cmd, defer, bpftrace, ethtool, rand_port


def _do_bpftrace(cfg, mul, base_size, tgt_queue=None):
    queue_filter = ''
    if tgt_queue is not None:
        queue_filter = 'if ($skb->queue_mapping != %d) {return;}' % (tgt_queue + 1, )

    t = ('tracepoint:net:netif_receive_skb { ' +
         '$skb = (struct sk_buff *)args->skbaddr; '+
         '$sh = (struct skb_shared_info *)($skb->head + $skb->end); ' +
         'if ($skb->dev->ifindex != ' + str(cfg.ifindex) + ') {return;} ' +
         queue_filter +
         '@[$skb->len - $skb->data_len] = count(); ' +
         '@h[$skb->len - $skb->data_len] = count(); ' +
         'if ($sh->nr_frags > 0) { @[$sh->frags[0].len] = count(); @d[$sh->frags[0].len] = count();} }'
        )
    maps = bpftrace(t, json=True, timeout=2)
    # We expect one-dim array with something like:
    # {"type": "map", "data": {"@": {"1500": 1, "719": 1,
    sizes = maps["@"]
    h = maps["@h"]
    d = maps["@d"]
    good = 0
    bad = 0
    for k, v in sizes.items():
        k = int(k)
        if mul == 1 and k > base_size:
            bad += v
        elif mul > 1 and k > base_size:
            good += v
        elif mul < 1 and k >= base_size:
            bad += v
    ksft_eq(bad, 0, "buffer was decreased but large buffers seen")
    if mul > 1:
        ksft_ge(good, 100, "buffer was increased but no large buffers seen")


def _ethtool_create(cfg, act, opts):
    output = ethtool(f"{act} {cfg.ifname} {opts}").stdout
    # Output will be something like: "New RSS context is 1" or
    # "Added rule with ID 7", we want the integer from the end
    return int(output.split()[-1])


def nic_wide(cfg, check_geometry=False) -> None:
    """
    Apply NIC wide rx-buf-len change. Run some traffic to make sure there
    are no crashes. Test that setting 0 restores driver default.
    Assume we start with the default.
    """
    try:
        rings = cfg.ethnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        rings = {}
    if "rx-buf-len" not in rings:
        raise KsftSkipEx('rx-buf-len configuration not supported by device')

    if rings['rx-buf-len'] * 2 <= rings['rx-buf-len-max']:
        mul = 2
    else:
        mul = 1/2
    cfg.ethnl.rings_set({'header': {'dev-index': cfg.ifindex},
                         'rx-buf-len': rings['rx-buf-len'] * mul})

    # Use zero to restore default, per uAPI, we assume we started with default
    reset = defer(cfg.ethnl.rings_set, {'header': {'dev-index': cfg.ifindex},
                                       'rx-buf-len': 0})

    new = cfg.ethnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    ksft_eq(new['rx-buf-len'], rings['rx-buf-len'] * mul, "config after change")

    # Runs some traffic thru them buffers, to make things implode if they do
    traf = GenerateTraffic(cfg)
    try:
        if check_geometry:
            _do_bpftrace(cfg, mul, rings['rx-buf-len'])
    finally:
        traf.wait_pkts_and_stop(20000)

    reset.exec()
    new = cfg.ethnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    ksft_eq(new['rx-buf-len'], rings['rx-buf-len'], "config reset to default")


def nic_wide_check_packets(cfg) -> None:
    nic_wide(cfg, check_geometry=True)


def _check_queues_with_config(cfg, buf_len, qset):
    cnt = 0
    queues = cfg.netnl.queue_get({'ifindex': cfg.ifindex}, dump=True)
    for q in queues:
        if 'rx-buf-len' in q:
            cnt += 1
            ksft_eq(q['type'], "rx")
            ksft_in(q['id'], qset)
            ksft_eq(q['rx-buf-len'], buf_len, "buf size")
    if cnt != len(qset):
        raise KsftFailEx('queue rx-buf-len config invalid')


def _per_queue_configure(cfg) -> Tuple[dict, int, defer]:
    """
    Prep for per queue test. Set the config on one queue and return
    the original ring settings, the multiplier and reset defer.
    """
    # Validate / get initial settings
    try:
        rings = cfg.ethnl.rings_get({'header': {'dev-index': cfg.ifindex}})
    except NlError as e:
        rings = {}
    if "rx-buf-len" not in rings:
        raise KsftSkipEx('rx-buf-len configuration not supported by device')

    try:
        queues = cfg.netnl.queue_get({'ifindex': cfg.ifindex}, dump=True)
    except NlError as e:
        raise KsftSkipEx('queue configuration not supported by device')

    if len(queues) < 2:
        raise KsftSkipEx('not enough queues: ' + str(len(queues)))
    for q in queues:
        if 'rx-buf-len' in q:
            raise KsftFailEx('queue rx-buf-len already configured')

    # Apply a change, we'll target queue 1
    if rings['rx-buf-len'] * 2 <= rings['rx-buf-len-max']:
        mul = 2
    else:
        mul = 1/2
    try:
        cfg.netnl.queue_set({'ifindex': cfg.ifindex, "type": "rx", "id": 1,
                             'rx-buf-len': rings['rx-buf-len'] * mul })
    except NlError as e:
        if e.error == errno.EOPNOTSUPP:
            raise KsftSkipEx('per-queue rx-buf-len configuration not supported')
        raise

    reset = defer(cfg.netnl.queue_set, {'ifindex': cfg.ifindex,
                                        "type": "rx", "id": 1,
                                        'rx-buf-len': 0})
    # Make sure config stuck
    _check_queues_with_config(cfg, rings['rx-buf-len'] * mul, {1})

    return rings, mul, reset


def per_queue(cfg, check_geometry=False) -> None:
    """
    Similar test to nic_wide, but done a single queue (queue 1).
    Flow filter is used to direct traffic to that queue.
    """

    rings, mul, reset = _per_queue_configure(cfg)
    _check_queues_with_config(cfg, rings['rx-buf-len'] * mul, {1})

    # Check with traffic, we need to direct the traffic to the expected queue
    port = rand_port()
    flow = f"flow-type tcp{cfg.addr_ipver} dst-ip {cfg.addr} dst-port {port} action 1"
    nid = _ethtool_create(cfg, "-N", flow)
    ntuple = defer(ethtool, f"-N {cfg.ifname} delete {nid}")

    traf = GenerateTraffic(cfg, port=port)
    try:
        if check_geometry:
            _do_bpftrace(cfg, mul, rings['rx-buf-len'], tgt_queue=1)
    finally:
        traf.wait_pkts_and_stop(20000)
    ntuple.exec()

    # And now direct to another queue
    flow = f"flow-type tcp{cfg.addr_ipver} dst-ip {cfg.addr} dst-port {port} action 0"
    nid = _ethtool_create(cfg, "-N", flow)
    ntuple = defer(ethtool, f"-N {cfg.ifname} delete {nid}")

    traf = GenerateTraffic(cfg, port=port)
    try:
        if check_geometry:
            _do_bpftrace(cfg, 1, rings['rx-buf-len'], tgt_queue=0)
    finally:
        traf.wait_pkts_and_stop(20000)

    # Back to default
    reset.exec()
    queues = cfg.netnl.queue_get({'ifindex': cfg.ifindex}, dump=True)
    for q in queues:
        ksft_not_in('rx-buf-len', q)


def per_queue_check_packets(cfg) -> None:
    per_queue(cfg, check_geometry=True)


def queue_check_ring_count(cfg, check_geometry=False) -> None:
    """
    Make sure the change of ring count is handled correctly.
    """
    rings, mul, reset = _per_queue_configure(cfg)

    channels = cfg.ethnl.channels_get({'header': {'dev-index': cfg.ifindex}})
    if channels.get('combined-count', 0) < 4:
        raise KsftSkipEx('need at least 4 rings, have',
                         channels.get('combined-count'))

    # Move the channel count up and down, should make no difference
    moves = [1, 0]
    if channels['combined-count'] == channels['combined-max']:
        moves = [-1, 0]
    for move in moves:
        target = channels['combined-count'] + move
        cfg.ethnl.channels_set({'header': {'dev-index': cfg.ifindex},
                                'combined-count': target})

    _check_queues_with_config(cfg, rings['rx-buf-len'] * mul, {1})

    # Check with traffic, we need to direct the traffic to the expected queue
    port1 = rand_port()
    flow1 = f"flow-type tcp{cfg.addr_ipver} dst-ip {cfg.addr} dst-port {port1} action 1"
    nid = _ethtool_create(cfg, "-N", flow1)
    ntuple = defer(ethtool, f"-N {cfg.ifname} delete {nid}")

    traf = GenerateTraffic(cfg, port=port1)
    try:
        if check_geometry:
            _do_bpftrace(cfg, mul, rings['rx-buf-len'], tgt_queue=1)
    finally:
        traf.wait_pkts_and_stop(20000)

    # And now direct to another queue
    port0 = rand_port()
    flow = f"flow-type tcp{cfg.addr_ipver} dst-ip {cfg.addr} dst-port {port0} action 0"
    nid = _ethtool_create(cfg, "-N", flow)
    defer(ethtool, f"-N {cfg.ifname} delete {nid}")

    traf = GenerateTraffic(cfg, port=port0)
    try:
        if check_geometry:
            _do_bpftrace(cfg, 1, rings['rx-buf-len'], tgt_queue=0)
    finally:
        traf.wait_pkts_and_stop(20000)

    # Go to a single queue, should reset
    ntuple.exec()
    cfg.ethnl.channels_set({'header': {'dev-index': cfg.ifindex},
                            'combined-count': 1})
    cfg.ethnl.channels_set({'header': {'dev-index': cfg.ifindex},
                            'combined-count': channels['combined-count']})

    nid = _ethtool_create(cfg, "-N", flow1)
    defer(ethtool, f"-N {cfg.ifname} delete {nid}")

    queues = cfg.netnl.queue_get({'ifindex': cfg.ifindex}, dump=True)
    for q in queues:
        ksft_not_in('rx-buf-len', q)

    # Check with traffic that queue is now getting normal buffers
    traf = GenerateTraffic(cfg, port=port1)
    try:
        if check_geometry:
            _do_bpftrace(cfg, 1, rings['rx-buf-len'], tgt_queue=1)
    finally:
        traf.wait_pkts_and_stop(20000)


def queue_check_ring_count_check_packets(cfg):
    queue_check_ring_count(cfg, True)


def main() -> None:
    with NetDrvEpEnv(__file__) as cfg:
        cfg.netnl = NetdevFamily()
        cfg.ethnl = EthtoolFamily()

        o = [nic_wide,
             per_queue,
             nic_wide_check_packets]

        ksft_run([nic_wide,
                  nic_wide_check_packets,
                  per_queue,
                  per_queue_check_packets,
                  queue_check_ring_count,
                  queue_check_ring_count_check_packets],
                 args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
