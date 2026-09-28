#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
import argparse
import os
from typing import List, Optional, Union
from metric import (aggr_nr, d_ratio, has_event, source_count, Event, Expression,
                    JsonEncodeMetric, JsonEncodeMetricGroupDescriptions, LoadEvents, Metric,
                    MetricGroup, Select)
from common_metrics import Cycles

# Global command line arguments.
_args = None


def NvidiaT410Uncore() -> Optional[MetricGroup]:
    """Uncore metrics for the NVIDIA Tegra410 SoC.

    The formulas follow Documentation/admin-guide/perf/nvidia-tegra410-pmu.rst.
    """
    assert _args is not None
    if _args.vendor != "nvidia" or _args.model != "t410":
        return None

    interval_sec = Event("duration_time")
    mb_per_sec = f"{1/1e6}MB/s"

    def DdrBw() -> MetricGroup:
        # Unified Coherence Fabric (UCF) PMU:
        # AVG_MEM_{READ,WRITE}_BANDWIDTH = MEM_BYTES_{RD,WR} / ELAPSED_TIME
        loc_src = "src_loc_cpu=0x1,src_loc_noncpu=0x1"
        rd = Event("nvidia_ucf_pmu/mem_bytes_rd,dst_loc_cmem=0x1/")
        wr = Event("nvidia_ucf_pmu/mem_bytes_wr,dst_loc_cmem=0x1/")
        loc_rd = Event(f"nvidia_ucf_pmu/mem_bytes_rd,{loc_src},dst_loc_cmem=0x1/")
        loc_wr = Event(f"nvidia_ucf_pmu/mem_bytes_wr,{loc_src},dst_loc_cmem=0x1/")
        rem_rd = Event(f"nvidia_ucf_pmu/mem_bytes_rd,{loc_src},dst_rem=0x1/")
        rem_wr = Event(f"nvidia_ucf_pmu/mem_bytes_wr,{loc_src},dst_rem=0x1/")
        return MetricGroup("lpm_ddr_bw", [
            Metric("lpm_ddr_bw", "Total DDR bandwidth",
                   d_ratio(rd + wr, interval_sec), mb_per_sec),
            Metric("lpm_ddr_rd_bw", "DDR read bandwidth",
                   d_ratio(rd, interval_sec), mb_per_sec),
            Metric("lpm_ddr_wr_bw", "DDR write bandwidth",
                   d_ratio(wr, interval_sec), mb_per_sec),
            Metric("lpm_ddr_loc_rd_bw", "Local DDR read bandwidth",
                   d_ratio(loc_rd, interval_sec), mb_per_sec),
            Metric("lpm_ddr_loc_wr_bw", "Local DDR write bandwidth",
                   d_ratio(loc_wr, interval_sec), mb_per_sec),
            Metric("lpm_ddr_rem_rd_bw", "Remote DDR read bandwidth",
                   d_ratio(rem_rd, interval_sec), mb_per_sec),
            Metric("lpm_ddr_rem_wr_bw", "Remote DDR write bandwidth",
                   d_ratio(rem_wr, interval_sec), mb_per_sec),
        ], description="NVIDIA Tegra410 DDR bandwidth")

    def DdrLat() -> MetricGroup:
        # CPU Memory (CMEM) latency PMU, one per socket:
        # AVERAGE_LATENCY_IN_NS = (RD_CUM_OUTS / RD_REQ) / (CYCLES / ELAPSED_TIME_IN_NS)
        rd_cum_outs = Event("nvidia_cmem_latency_pmu/rd_cum_outs/")
        rd_req = Event("nvidia_cmem_latency_pmu/rd_req/")
        cycles = Event("nvidia_cmem_latency_pmu/cycles/")
        # perf sums the counts of the PMUs in an aggregation, e.g. both sockets
        # by default, so divide cycles by their number to get the frequency.
        rd_lat = d_ratio(1e9 * interval_sec * rd_cum_outs * aggr_nr(cycles), rd_req * cycles)
        return MetricGroup("lpm_ddr_lat", [
            Metric("lpm_ddr_lat", "Average DDR read latency in ns", rd_lat, "1ns"),
            Metric("lpm_ddr_rd_lat", "DDR read latency in ns", rd_lat, "1ns"),
            # The CMEM latency PMU only measures reads, write latency is always 0.
            Metric("lpm_ddr_wr_lat", "DDR write latency in ns",
                   rd_cum_outs - rd_cum_outs, "1ns"),
        ], description="NVIDIA Tegra410 DDR latency")

    def PcieBw() -> MetricGroup:
        # PCIE PMU, one per socket and PCIe Root Complex (RC):
        # AVG_{RD,WR}_BANDWIDTH = {RD,WR}_BYTES / ELAPSED_TIME
        loc_dst = "dst_loc_cmem=0x1,dst_loc_gmem=0x1,dst_loc_pcie_p2p=0x1,dst_loc_pcie_cxl=0x1"
        rem_dst = "dst_rem=0x1"

        def SocketSum(rc: str, event: str) -> Expression:
            # Sum the event over both sockets. A PMU counts as 0 if it isn't
            # present (has_event() is 0) or isn't counted in the aggregation,
            # e.g. the other socket with --per-socket (source_count() is 0).
            def Count(e: Event) -> Expression:
                return Select(Select(e, source_count(e), 0), has_event(e), 0)

            return (Count(Event(f"nvidia_pcie_pmu_0_{rc}/{event}/")) +
                    Count(Event(f"nvidia_pcie_pmu_1_{rc}/{event}/")))

        # nvidia_pcie_pmu_<socket-id>_rc matches the PMUs of all RCs in the socket.
        rd = SocketSum("rc", "rd_bytes")
        wr = SocketSum("rc", "wr_bytes")
        metrics: List[Union[Metric, MetricGroup]] = [
            Metric("lpm_pcie_bw", "Total PCIe bandwidth across all Root Complexes",
                   d_ratio(rd + wr, interval_sec), mb_per_sec),
            Metric("lpm_pcie_rd_bw", "Total PCIe read bandwidth across all Root Complexes",
                   d_ratio(rd, interval_sec), mb_per_sec),
            Metric("lpm_pcie_wr_bw", "Total PCIe write bandwidth across all Root Complexes",
                   d_ratio(wr, interval_sec), mb_per_sec),
        ]
        for i in range(6):
            rc = f"rc_{i}"
            rc_rd = SocketSum(rc, "rd_bytes")
            rc_wr = SocketSum(rc, "wr_bytes")
            loc_rd = SocketSum(rc, f"rd_bytes,{loc_dst}")
            loc_wr = SocketSum(rc, f"wr_bytes,{loc_dst}")
            rem_rd = SocketSum(rc, f"rd_bytes,{rem_dst}")
            rem_wr = SocketSum(rc, f"wr_bytes,{rem_dst}")
            metrics.append(MetricGroup(f"lpm_pcie_rc{i}_bw", [
                Metric(f"lpm_pcie_bw_{i}", f"PCIe total bandwidth on Root Complex {i}",
                       d_ratio(rc_rd + rc_wr, interval_sec), mb_per_sec),
                Metric(f"lpm_pcie_rd_bw_{i}", f"PCIe read bandwidth on Root Complex {i}",
                       d_ratio(rc_rd, interval_sec), mb_per_sec),
                Metric(f"lpm_pcie_wr_bw_{i}", f"PCIe write bandwidth on Root Complex {i}",
                       d_ratio(rc_wr, interval_sec), mb_per_sec),
                Metric(f"lpm_pcie_loc_rd_bw_{i}",
                       f"PCIe local read bandwidth on Root Complex {i}",
                       d_ratio(loc_rd, interval_sec), mb_per_sec),
                Metric(f"lpm_pcie_loc_wr_bw_{i}",
                       f"PCIe local write bandwidth on Root Complex {i}",
                       d_ratio(loc_wr, interval_sec), mb_per_sec),
                Metric(f"lpm_pcie_rem_rd_bw_{i}",
                       f"PCIe remote read bandwidth on Root Complex {i}",
                       d_ratio(rem_rd, interval_sec), mb_per_sec),
                Metric(f"lpm_pcie_rem_wr_bw_{i}",
                       f"PCIe remote write bandwidth on Root Complex {i}",
                       d_ratio(rem_wr, interval_sec), mb_per_sec),
            ], description=f"PCIe bandwidth on Root Complex {i}"))

        return MetricGroup("lpm_pcie_bw", metrics,
                           description="NVIDIA Tegra410 PCIe Root Complex bandwidth")

    return MetricGroup("lpm_uncore", [DdrBw(), DdrLat(), PcieBw()],
                       description="NVIDIA Tegra410 uncore metrics")


def main() -> None:
    global _args

    def dir_path(path: str) -> str:
        """Validate path is a directory for argparse."""
        if os.path.isdir(path):
            return path
        raise argparse.ArgumentTypeError(
            f'\'{path}\' is not a valid directory')

    parser = argparse.ArgumentParser(description="ARM perf json generator")
    parser.add_argument(
        "-metricgroups", help="Generate metricgroups data", action='store_true')
    parser.add_argument("vendor", help="e.g. arm")
    parser.add_argument("model", help="e.g. neoverse-n1")
    parser.add_argument(
        'events_path',
        type=dir_path,
        help='Root of tree containing architecture directories containing json files'
    )
    _args = parser.parse_args()

    directory = f"{_args.events_path}/arm64/{_args.vendor}/{_args.model}/"
    LoadEvents(directory)

    all_metrics = MetricGroup("", [
        Cycles(),
        NvidiaT410Uncore(),
    ])

    if _args.metricgroups:
        print(JsonEncodeMetricGroupDescriptions(all_metrics))
    else:
        print(JsonEncodeMetric(all_metrics))


if __name__ == '__main__':
    main()
