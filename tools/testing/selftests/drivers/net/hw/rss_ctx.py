#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import datetime
import random
from lib.py import ksft_run, ksft_pr, ksft_exit, ksft_eq, ksft_ge, ksft_lt
from lib.py import NetDrvEpEnv
from lib.py import NetdevFamily
from lib.py import KsftSkipEx
from lib.py import rand_port
from lib.py import ethtool, ip, GenerateTraffic, CmdExitFailure


def _rss_key_str(key):
    return ":".join(["{:02x}".format(x) for x in key])


def _rss_key_rand(length):
    return [random.randint(0, 255) for _ in range(length)]


def get_rss(cfg):
    return ethtool(f"-x {cfg.ifname}", json=True)[0]


def ethtool_create(cfg, act, opts):
    output = ethtool(f"{act} {cfg.ifname} {opts}").stdout
    # Output will be something like: "New RSS context is 1" or
    # "Added rule with ID 7", we want the integer from the end
    return int(output.split()[-1])


# Get Rx packet counts for all queues, as a simple list of integers
# if @prev is specified the prev counts will be subtracted
def _get_rx_cnts(cfg, prev=None):
    cfg.wait_hw_stats_settle()
    data = cfg.netdevnl.qstats_get({"ifindex": cfg.ifindex, "scope": ["queue"]}, dump=True)
    data = [x for x in data if x['queue-type'] == "rx"]
    max_q = max([x["queue-id"] for x in data])
    queue_stats = [0] * (max_q + 1)
    for q in data:
        queue_stats[q["queue-id"]] = q["rx-packets"]
        if prev and q["queue-id"] < len(prev):
            queue_stats[q["queue-id"]] -= prev[q["queue-id"]]
    return queue_stats


def test_rss_key_indir(cfg):
    """
    Test basics like updating the main RSS key and indirection table.
    """
    data = get_rss(cfg)
    want_keys = ['rss-hash-key', 'rss-hash-function', 'rss-indirection-table']
    for k in want_keys:
        if k not in data:
            raise KsftFailEx("ethtool results missing key: " + k)
        if not data[k]:
            raise KsftFailEx(f"ethtool results empty for '{k}': {data[k]}")

    key_len = len(data['rss-hash-key'])

    # Set the key
    key = _rss_key_rand(key_len)
    ethtool(f"-X {cfg.ifname} hkey " + _rss_key_str(key))

    data = get_rss(cfg)
    ksft_eq(key, data['rss-hash-key'])

    # Set the indirection table
    ethtool(f"-X {cfg.ifname} equal 2")
    data = get_rss(cfg)
    ksft_eq(0, min(data['rss-indirection-table']))
    ksft_eq(1, max(data['rss-indirection-table']))

    # Check we only get traffic on the first 2 queues
    cnts = _get_rx_cnts(cfg)
    GenerateTraffic(cfg).wait_pkts_and_stop(20000)
    cnts = _get_rx_cnts(cfg, prev=cnts)
    # 2 queues, 20k packets, must be at least 5k per queue
    ksft_ge(cnts[0], 5000, "traffic on main context (1/2): " + str(cnts))
    ksft_ge(cnts[1], 5000, "traffic on main context (2/2): " + str(cnts))
    # The other queues should be unused
    ksft_eq(sum(cnts[2:]), 0, "traffic on unused queues: " + str(cnts))

    # Restore, and check traffic gets spread again
    ethtool(f"-X {cfg.ifname} default")

    cnts = _get_rx_cnts(cfg)
    GenerateTraffic(cfg).wait_pkts_and_stop(20000)
    cnts = _get_rx_cnts(cfg, prev=cnts)
    # First two queues get less traffic than all the rest
    ksft_ge(sum(cnts[2:]), sum(cnts[:2]), "traffic distributed: " + str(cnts))


def test_rss_context(cfg, ctx_cnt=1):
    """
    Test separating traffic into RSS contexts.
    The queues will be allocated 2 for each context:
     ctx0  ctx1  ctx2  ctx3
    [0 1] [2 3] [4 5] [6 7] ...
    """

    requested_ctx_cnt = ctx_cnt

    # Try to allocate more queues when necessary
    qcnt = len(_get_rx_cnts(cfg))
    if qcnt >= 2 + 2 * ctx_cnt:
        qcnt = None
    else:
        try:
            ksft_pr(f"Increasing queue count {qcnt} -> {2 + 2 * ctx_cnt}")
            ethtool(f"-L {cfg.ifname} combined {2 + 2 * ctx_cnt}")
        except:
            raise KsftSkipEx("Not enough queues for the test")

    ntuple = []
    ctx_id = []
    ports = []
    try:
        # Use queues 0 and 1 for normal traffic
        ethtool(f"-X {cfg.ifname} equal 2")

        for i in range(ctx_cnt):
            try:
                ctx_id.append(ethtool_create(cfg, "-X", "context new"))
            except CmdExitFailure:
                # try to carry on and skip at the end
                if i == 0:
                    raise
                ksft_pr(f"Failed to create context {i + 1}, trying to test what we got")
                ctx_cnt = i
                break

            ethtool(f"-X {cfg.ifname} context {ctx_id[i]} start {2 + i * 2} equal 2")

            ports.append(rand_port())
            flow = f"flow-type tcp{cfg.addr_ipver} dst-port {ports[i]} context {ctx_id[i]}"
            ntuple.append(ethtool_create(cfg, "-N", flow))

        for i in range(ctx_cnt):
            cnts = _get_rx_cnts(cfg)
            GenerateTraffic(cfg, port=ports[i]).wait_pkts_and_stop(20000)
            cnts = _get_rx_cnts(cfg, prev=cnts)

            ksft_lt(sum(cnts[ :2]), 10000, "traffic on main context:" + str(cnts))
            ksft_ge(sum(cnts[2+i*2:4+i*2]), 20000, f"traffic on context {i}: " + str(cnts))
            ksft_eq(sum(cnts[2:2+i*2] + cnts[4+i*2:]), 0, "traffic on other contexts: " + str(cnts))
    finally:
        for nid in ntuple:
            ethtool(f"-N {cfg.ifname} delete {nid}")
        for cid in ctx_id:
            ethtool(f"-X {cfg.ifname} context {cid} delete")
        ethtool(f"-X {cfg.ifname} default")
        if qcnt:
            ethtool(f"-L {cfg.ifname} combined {qcnt}")

    if requested_ctx_cnt != ctx_cnt:
        raise KsftSkipEx(f"Tested only {ctx_cnt} contexts, wanted {requested_ctx_cnt}")


def test_rss_context4(cfg):
    test_rss_context(cfg, 4)


def test_rss_context32(cfg):
    test_rss_context(cfg, 32)


def test_rss_context_overlap(cfg, other_ctx=0):
    """
    Test contexts overlapping with each other.
    Use 4 queues for the main context, but only queues 2 and 3 for context 1.
    """
    ctx_id = None
    ntuple = None
    if other_ctx == 0:
        ethtool(f"-X {cfg.ifname} equal 4")
    else:
        other_ctx = ethtool_create(cfg, "-X", "context new")
        ethtool(f"-X {cfg.ifname} context {other_ctx} equal 4")

    try:
        ctx_id = ethtool_create(cfg, "-X", "context new")
        ethtool(f"-X {cfg.ifname} context {ctx_id} start 2 equal 2")

        port = rand_port()
        if other_ctx:
            flow = f"flow-type tcp{cfg.addr_ipver} dst-port {port} context {other_ctx}"
            ntuple = ethtool_create(cfg, "-N", flow)

        # Test the main context
        cnts = _get_rx_cnts(cfg)
        GenerateTraffic(cfg, port=port).wait_pkts_and_stop(20000)
        cnts = _get_rx_cnts(cfg, prev=cnts)

        ksft_ge(sum(cnts[ :4]), 20000, "traffic on main context: " + str(cnts))
        ksft_ge(sum(cnts[ :2]),  7000, "traffic on main context (1/2): " + str(cnts))
        ksft_ge(sum(cnts[2:4]),  7000, "traffic on main context (2/2): " + str(cnts))
        if other_ctx == 0:
            ksft_eq(sum(cnts[4: ]),     0, "traffic on other queues: " + str(cnts))

        # Now create a rule for context 1 and make sure traffic goes to a subset
        if other_ctx:
            ethtool(f"-N {cfg.ifname} delete {ntuple}")
        flow = f"flow-type tcp{cfg.addr_ipver} dst-port {port} context {ctx_id}"
        ntuple = ethtool_create(cfg, "-N", flow)

        cnts = _get_rx_cnts(cfg)
        GenerateTraffic(cfg, port=port).wait_pkts_and_stop(20000)
        cnts = _get_rx_cnts(cfg, prev=cnts)

        ksft_lt(sum(cnts[ :2]),  7000, "traffic on main context: " + str(cnts))
        ksft_ge(sum(cnts[2:4]), 20000, "traffic on extra context: " + str(cnts))
        if other_ctx == 0:
            ksft_eq(sum(cnts[4: ]),     0, "traffic on other queues: " + str(cnts))
    finally:
        if ntuple:
            ethtool(f"-N {cfg.ifname} delete {ntuple}")
        if ctx_id:
            ethtool(f"-X {cfg.ifname} context {ctx_id} delete")
        if other_ctx == 0:
            ethtool(f"-X {cfg.ifname} default")
        else:
            ethtool(f"-X {cfg.ifname} context {other_ctx} delete")


def test_rss_context_overlap2(cfg):
    test_rss_context_overlap(cfg, True)


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.netdevnl = NetdevFamily()

        ksft_run([test_rss_key_indir, test_rss_context, test_rss_context4,
                  test_rss_context32, test_rss_context_overlap,
                  test_rss_context_overlap2],
                 args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
