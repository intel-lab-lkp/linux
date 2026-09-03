#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
GRO (Generic Receive Offload) conformance tests.

Validates that GRO coalescing works correctly by running the gro
binary in different configurations and checking for correct packet
coalescing behavior.

Test cases:
  - data_same: Same size data packets coalesce
  - data_lrg_sml: Large packet followed by smaller one coalesces
  - data_lrg_1byte: Large packet followed by 1B one coalesces (Ethernet padding)
  - data_sml_lrg: Small packet followed by larger one doesn't coalesce
  - ack: Pure ACK packets do not coalesce
  - flags_psh: Packets with PSH flag don't coalesce
  - flags_syn: Packets with SYN flag don't coalesce
  - flags_rst: Packets with RST flag don't coalesce
  - flags_urg: Packets with URG flag don't coalesce
  - flags_cwr: Packets with CWR flag don't coalesce
  - tcp_csum: Packets with incorrect checksum don't coalesce
  - tcp_seq: Packets with non-consecutive seqno don't coalesce
  - tcp_ts: Packets with different timestamp options don't coalesce
  - tcp_opt: Packets with different TCP options don't coalesce
  - ip_ecn: Packets with different ECN don't coalesce
  - ip_tos: Packets with different TOS don't coalesce
  - ip_ttl: (IPv4) Packets with different TTL don't coalesce
  - ip_opt: (IPv4) Packets with IP options don't coalesce
  - ip_frag4: (IPv4) IPv4 fragments don't coalesce
  - ip_id_df*: (IPv4) IP ID field coalescing tests
  - ip_frag6: (IPv6) IPv6 fragments don't coalesce
  - ip_v6ext_same: (IPv6) IPv6 ext header with same payload coalesces
  - ip_v6ext_diff: (IPv6) IPv6 ext header with different payload doesn't coalesce
  - large_max: Packets exceeding GRO_MAX_SIZE don't coalesce
  - large_rem: Large packet remainder handling
"""

import glob
import re
from lib.py import ksft_run, ksft_exit, ksft_pr
from lib.py import NetDrvEpEnv, KsftXfailEx
from lib.py import NetdevFamily, EthtoolFamily
from lib.py import defer, ethtool
from lib.py import ksft_variants, KsftNamedVariant
from gro_lib import run_gro_bin, run_with_retries, set_ethtool_feat
from gro_lib import set_mtu_restore, setup_hw_gro, write_defer_restore


# gro.c uses hardcoded DPORT=8000
GRO_DPORT = 8000


def _get_queue_stats(cfg, queue_id):
    """Get stats for a specific Rx queue."""
    cfg.wait_hw_stats_settle()
    data = cfg.netnl.qstats_get({"ifindex": cfg.ifindex, "scope": ["queue"]},
                                dump=True)
    for q in data:
        if q.get('queue-type') == 'rx' and q.get('queue-id') == queue_id:
            return q
    return {}


def _require_ntuple(cfg):
    features = ethtool(f"-k {cfg.ifname}", json=True)[0]
    if not features["ntuple-filters"]["active"]:
        if features["ntuple-filters"]["fixed"]:
            raise KsftXfailEx("Device does not support ntuple-filters")
        ethtool(f"-K {cfg.ifname} ntuple-filters on")
        defer(ethtool, f"-K {cfg.ifname} ntuple-filters off")


def _setup_isolated_queue(cfg):
    """Set up an isolated queue for testing using ntuple filter.

    Remove queue 1 from the default RSS context and steer test traffic to it.
    """
    _require_ntuple(cfg)
    test_queue = 1

    qcnt = len(glob.glob(f"/sys/class/net/{cfg.ifname}/queues/rx-*"))
    if qcnt < 2:
        raise KsftXfailEx(f"Need at least 2 queues, have {qcnt}")

    # Remove queue 1 from default RSS context by setting its weight to 0
    weights = ["1"] * qcnt
    weights[test_queue] = "0"
    ethtool(f"-X {cfg.ifname} weight " + " ".join(weights))
    defer(ethtool, f"-X {cfg.ifname} default")

    # Set up ntuple filter to steer our test traffic to the isolated queue
    flow  = f"flow-type tcp{cfg.addr_ipver} "
    flow += f"dst-ip {cfg.addr} dst-port {GRO_DPORT} action {test_queue}"
    output = ethtool(f"-N {cfg.ifname} {flow}").stdout
    ntuple_id = int(output.split()[-1])
    defer(ethtool, f"-N {cfg.ifname} delete {ntuple_id}")

    return test_queue


def _setup_queue_count(cfg, num_queues):
    """Configure the NIC to use a specific number of queues."""
    channels = cfg.ethnl.channels_get({'header': {'dev-index': cfg.ifindex}})
    ch_max = channels.get('combined-max', 0)
    qcnt = channels['combined-count']

    if ch_max < num_queues:
        raise KsftXfailEx(f"Need at least {num_queues} queues, max={ch_max}")

    defer(ethtool, f"-L {cfg.ifname} combined {qcnt}")
    ethtool(f"-L {cfg.ifname} combined {num_queues}")


def _setup(cfg, mode, test_name):
    """ Setup hardware loopback mode for GRO testing. """

    if not hasattr(cfg, "bin_remote"):
        cfg.bin_local = cfg.net_lib_dir / "gro"
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

    if not hasattr(cfg, "feat"):
        cfg.feat = ethtool(f"-k {cfg.ifname}", json=True)[0]
        cfg.remote_feat = ethtool(f"-k {cfg.remote_ifname}",
                                  host=cfg.remote, json=True)[0]

    # "large_*" tests need at least 4k MTU
    if test_name.startswith("large_"):
        set_mtu_restore(cfg.dev, 4096, None)
        set_mtu_restore(cfg.remote_dev, 4096, cfg.remote)

    if mode == "sw":
        flush_path = f"/sys/class/net/{cfg.ifname}/gro_flush_timeout"
        irq_path = f"/sys/class/net/{cfg.ifname}/napi_defer_hard_irqs"

        write_defer_restore(cfg, flush_path, "200000", defer_undo=True)
        write_defer_restore(cfg, irq_path, "10", defer_undo=True)

        set_ethtool_feat(cfg.ifname, cfg.feat,
                         {"generic-receive-offload": True,
                          "rx-gro-hw": False,
                          "large-receive-offload": False})
    elif mode == "hw":
        setup_hw_gro(cfg)
    elif mode == "lro":
        # netdevsim advertises LRO for feature inheritance testing with
        # bonding/team tests but it doesn't actually perform the offload
        cfg.require_nsim(nsim_test=False)

        set_ethtool_feat(cfg.ifname, cfg.feat,
                         {"generic-receive-offload": False,
                          "rx-gro-hw": False,
                          "large-receive-offload": True})

    try:
        # Disable TSO for local tests
        cfg.require_nsim()  # will raise KsftXfailEx if not running on nsim

        set_ethtool_feat(cfg.remote_ifname, cfg.remote_feat,
                         {"tcp-segmentation-offload": False},
                         host=cfg.remote)
    except KsftXfailEx:
        pass


def _gro_variants():
    """Generator that yields all combinations of protocol and test types."""

    # Tests that work for all protocols
    common_tests = [
        "data_same", "data_lrg_sml", "data_sml_lrg", "data_lrg_1byte",
        "data_burst",
        "ack",
        "flags_psh", "flags_syn", "flags_rst", "flags_urg", "flags_cwr",
        "tcp_csum", "tcp_seq", "tcp_ts", "tcp_opt",
        "ip_ecn", "ip_tos",
        "large_max", "large_rem",
    ]

    # Tests specific to IPv4
    ipv4_tests = [
        "ip_csum",
        "ip_ttl", "ip_opt", "ip_frag4",
        "ip_id_df1_inc", "ip_id_df1_fixed",
        "ip_id_df0_inc", "ip_id_df0_fixed",
        "ip_id_df1_inc_fixed", "ip_id_df1_fixed_inc",
    ]

    # Tests specific to IPv6
    ipv6_tests = [
        "ip_frag6", "ip_v6ext_same", "ip_v6ext_diff",
    ]

    # Tests specific to PPPoE
    pppoe_tests = [
        "data_same", "data_lrg_sml", "data_sml_lrg", "data_lrg_1byte",
        "data_burst", "pppoe_sid",
    ]

    for mode in ["sw", "hw", "lro"]:
        for protocol in ["ipv4", "ipv6", "ipip", "ip6ip6"]:
            for test_name in common_tests:
                yield mode, protocol, test_name

            if protocol in ["ipv4", "ipip"]:
                for test_name in ipv4_tests:
                    yield mode, protocol, test_name
            elif protocol == "ipv6":
                for test_name in ipv6_tests:
                    yield mode, protocol, test_name

    for mode in ["sw"]:
        for protocol in ["pppoev4", "pppoev6"]:
            for test_name in pppoe_tests:
                yield mode, protocol, test_name


@ksft_variants(_gro_variants())
def test(cfg, mode, protocol, test_name):
    """Run a single GRO test with retries."""

    ipver = "6" if protocol[-1] == "6" else "4"
    cfg.require_ipver(ipver)

    _setup(cfg, mode, test_name)

    run_with_retries(cfg, test_name, protocol=protocol, verbose=True)


def _capacity_variants():
    """Generate variants for capacity test: mode x queue setup."""
    setups = [
        ("isolated", _setup_isolated_queue),
        ("1q", lambda cfg: _setup_queue_count(cfg, 1)),
        ("8q", lambda cfg: _setup_queue_count(cfg, 8)),
    ]
    for mode in ["sw", "hw", "lro"]:
        for name, func in setups:
            yield KsftNamedVariant(f"{mode}_{name}", mode, func)


@ksft_variants(_capacity_variants())
def test_gro_capacity(cfg, mode, setup_func):
    """
    Probe GRO capacity.

    Start with 8 flows and increase by 2x on each successful run.
    Retry up to 3 times on failure.

    Variants combine mode (sw, hw, lro) with queue setup:
      - isolated: Use a single queue isolated from RSS
      - 1q: Configure NIC to use 1 queue
      - 8q: Configure NIC to use 8 queues
    """
    max_retries = 3

    _setup(cfg, mode, "capacity")
    queue_id = setup_func(cfg)

    num_flows = 8
    while True:
        success = False
        for attempt in range(max_retries):
            if queue_id is not None:
                stats_before = _get_queue_stats(cfg, queue_id)

            rx_proc = run_gro_bin(cfg, "capacity", num_flows=num_flows)
            output = rx_proc.stdout

            if queue_id is not None:
                stats_after = _get_queue_stats(cfg, queue_id)
                qstat_pkts = (stats_after.get('rx-packets', 0) -
                              stats_before.get('rx-packets', 0))
                gro_pkts = (stats_after.get('rx-hw-gro-packets', 0) -
                            stats_before.get('rx-hw-gro-packets', 0))
                qstat_str = f" qstat={qstat_pkts} hw-gro={gro_pkts}"
            else:
                qstat_str = ""

            # Parse and print STATS line
            match = re.search(
                r'STATS: received=(\d+) wire=(\d+) coalesced=(\d+)', output)
            if match:
                received = int(match.group(1))
                wire = int(match.group(2))
                coalesced = int(match.group(3))
                status = "PASS" if received == num_flows else "MISS"
                ksft_pr(f"flows={num_flows} attempt={attempt + 1} "
                        f"received={received} wire={wire} "
                        f"coalesced={coalesced}{qstat_str} [{status}]")
                if received == num_flows:
                    success = True
                    break
            else:
                ksft_pr(rx_proc)
                ksft_pr(f"flows={num_flows} attempt={attempt + 1}"
                        f"{qstat_str} [FAIL - can't parse stats]")

        if not success:
            ksft_pr(f"Stopped at {num_flows} flows")
            break

        num_flows *= 2


def main() -> None:
    """ Ksft boiler plate main """

    with NetDrvEpEnv(__file__) as cfg:
        cfg.ethnl = EthtoolFamily()
        cfg.netnl = NetdevFamily()
        ksft_run(cases=[test, test_gro_capacity], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
