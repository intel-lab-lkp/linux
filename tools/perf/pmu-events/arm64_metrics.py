#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
import argparse
import os
from typing import Optional
from metric import (aggr_nr, d_ratio, Event, JsonEncodeMetric,
                    JsonEncodeMetricGroupDescriptions, LoadEvents, Metric, MetricGroup)
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

    return MetricGroup("lpm_uncore", [DdrBw(), DdrLat()],
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
