#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
import argparse
import os
from typing import Optional
from metric import (d_ratio, Event, JsonEncodeMetric, JsonEncodeMetricGroupDescriptions,
                    LoadEvents, Metric, MetricGroup)

# Global command line arguments.
_args = None


def Arm64Topdown() -> MetricGroup:
    """Returns a MetricGroup representing ARM64 topdown like metrics."""
    def TryEvent(name: str) -> Optional[Event]:
        # Skip an event if not in the json files.
        try:
            return Event(name)
        except:
            return None
    # ARM models like a53 lack JSON for INST_RETIRED but have the
    # architetural standard event in sysfs. Use the PMU name to identify
    # the sysfs event.
    pmu_name = f'armv8_{_args.model.replace("-", "_")}'
    ins = Event("instructions")
    ins_ret = Event("INST_RETIRED", f"{pmu_name}/inst_retired/")
    cycles = Event("cpu\\-cycles")
    stall_fe = TryEvent("STALL_FRONTEND")
    stall_be = TryEvent("STALL_BACKEND")
    br_ret = TryEvent("BR_RETIRED")
    br_mp_ret = TryEvent("BR_MIS_PRED_RETIRED")
    dtlb_walk = TryEvent("DTLB_WALK")
    itlb_walk = TryEvent("ITLB_WALK")
    l1d_tlb = TryEvent("L1D_TLB")
    l1i_tlb = TryEvent("L1I_TLB")
    l1d_refill = Event("L1D_CACHE_REFILL", f"{pmu_name}/l1d_cache_refill/")
    l2d_refill = Event("L2D_CACHE_REFILL", f"{pmu_name}/l2d_cache_refill/")
    l1i_refill = Event("L1I_CACHE_REFILL", f"{pmu_name}/l1i_cache_refill/")
    l1d_access = Event("L1D_CACHE", f"{pmu_name}/l1d_cache/")
    l2d_access = Event("L2D_CACHE", f"{pmu_name}/l2d_cache/")
    llc_access = TryEvent("LL_CACHE_RD")
    l1i_access = Event("L1I_CACHE", f"{pmu_name}/l1i_cache/")
    llc_miss_rd = TryEvent("LL_CACHE_MISS_RD")
    ase_spec = TryEvent("ASE_SPEC")
    ld_spec = TryEvent("LD_SPEC")
    st_spec = TryEvent("ST_SPEC")
    vfp_spec = TryEvent("VFP_SPEC")
    dp_spec = TryEvent("DP_SPEC")
    br_immed_spec = TryEvent("BR_IMMED_SPEC")
    br_indirect_spec = TryEvent("BR_INDIRECT_SPEC")
    br_ret_spec = TryEvent("BR_RETURN_SPEC")
    crypto_spec = TryEvent("CRYPTO_SPEC")
    inst_spec = TryEvent("INST_SPEC")
    return MetricGroup("lpm_topdown", [
        MetricGroup("lpm_topdown_tl", [
            Metric("lpm_topdown_tl_ipc", "Instructions per cycle", d_ratio(
                ins, cycles), "insn/cycle"),
            Metric("lpm_topdown_tl_stall_fe_rate", "Frontend stalls to all cycles",
                   d_ratio(stall_fe, cycles), "100%") if stall_fe else None,
            Metric("lpm_topdown_tl_stall_be_rate", "Backend stalls to all cycles",
                   d_ratio(stall_be, cycles), "100%") if stall_be else None,
        ]),
        MetricGroup("lpm_topdown_fe_bound", [
            MetricGroup("lpm_topdown_fe_br", [
                Metric("lpm_topdown_fe_br_mp_per_insn",
                       "Branch mispredicts per instruction retired",
                       d_ratio(br_mp_ret, ins_ret), "br/insn") if br_mp_ret else None,
                Metric("lpm_topdown_fe_br_ins_rate",
                       "Branches per instruction retired", d_ratio(
                           br_ret, ins_ret), "100%") if br_ret else None,
                Metric("lpm_topdown_fe_br_mispredict",
                       "Branch mispredicts per branch instruction",
                       d_ratio(br_mp_ret, br_ret), "100%") if (br_mp_ret and br_ret) else None,
            ]),
            MetricGroup("lpm_topdown_fe_itlb", [
                Metric("lpm_topdown_fe_itlb_walks", "Itlb walks per insn",
                       d_ratio(itlb_walk, ins_ret), "walk/insn"),
                Metric("lpm_topdown_fe_itlb_walk_rate", "Itlb walks per L1I TLB access",
                       d_ratio(itlb_walk, l1i_tlb) if l1i_tlb else None, "100%"),
            ]) if itlb_walk else None,
            MetricGroup("lpm_topdown_fe_icache", [
                Metric("lpm_topdown_fe_icache_l1i_per_insn",
                       "L1I cache refills per instruction",
                       d_ratio(l1i_refill, ins_ret), "l1i/insn"),
                Metric("lpm_topdown_fe_icache_l1i_miss_rate",
                       "L1I cache refills per L1I cache access",
                       d_ratio(l1i_refill, l1i_access), "100%"),
            ]),
        ]),
        MetricGroup("lpm_topdown_be_bound", [
            MetricGroup("lpm_topdown_be_dtlb", [
                Metric("lpm_topdown_be_dtlb_walks", "Dtlb walks per instruction",
                       d_ratio(dtlb_walk, ins_ret), "walk/insn"),
                Metric("lpm_topdown_be_dtlb_walk_rate", "Dtlb walks per L1D TLB access",
                       d_ratio(dtlb_walk, l1d_tlb) if l1d_tlb else None, "100%"),
            ]) if dtlb_walk else None,
            MetricGroup("lpm_topdown_be_mix", [
                Metric("lpm_topdown_be_mix_ld", "Percentage of load instructions",
                       d_ratio(ld_spec, inst_spec), "100%") if ld_spec else None,
                Metric("lpm_topdown_be_mix_st", "Percentage of store instructions",
                       d_ratio(st_spec, inst_spec), "100%") if st_spec else None,
                Metric("lpm_topdown_be_mix_simd", "Percentage of SIMD instructions",
                       d_ratio(ase_spec, inst_spec), "100%") if ase_spec else None,
                Metric("lpm_topdown_be_mix_fp",
                       "Percentage of floating point instructions",
                       d_ratio(vfp_spec, inst_spec), "100%") if vfp_spec else None,
                Metric("lpm_topdown_be_mix_dp",
                       "Percentage of data processing instructions",
                       d_ratio(dp_spec, inst_spec), "100%") if dp_spec else None,
                Metric("lpm_topdown_be_mix_crypto",
                       "Percentage of data processing instructions",
                       d_ratio(crypto_spec, inst_spec), "100%") if crypto_spec else None,
                Metric(
                    "lpm_topdown_be_mix_br", "Percentage of branch instructions",
                    d_ratio(br_immed_spec + br_indirect_spec + br_ret_spec,
                            inst_spec), "100%") if br_immed_spec and br_indirect_spec and br_ret_spec else None,
            ], description="Breakdown of instructions by type. Counts include both useful and wasted speculative instructions"
            ) if inst_spec else None,
            MetricGroup("lpm_topdown_be_dcache", [
                MetricGroup("lpm_topdown_be_dcache_l1", [
                    Metric("lpm_topdown_be_dcache_l1_per_insn",
                           "L1D cache refills per instruction",
                           d_ratio(l1d_refill, ins_ret), "refills/insn"),
                    Metric("lpm_topdown_be_dcache_l1_miss_rate",
                           "L1D cache refills per L1D cache access",
                           d_ratio(l1d_refill, l1d_access), "100%")
                ]),
                MetricGroup("lpm_topdown_be_dcache_l2", [
                    Metric("lpm_topdown_be_dcache_l2_per_insn",
                           "L2D cache refills per instruction",
                           d_ratio(l2d_refill, ins_ret), "refills/insn"),
                    Metric("lpm_topdown_be_dcache_l2_miss_rate",
                           "L2D cache refills per L2D cache access",
                           d_ratio(l2d_refill, l2d_access), "100%")
                ]),
                MetricGroup("lpm_topdown_be_dcache_llc", [
                    Metric("lpm_topdown_be_dcache_llc_per_insn",
                           "Last level cache misses per instruction",
                           d_ratio(llc_miss_rd, ins_ret), "miss/insn"),
                    Metric("lpm_topdown_be_dcache_llc_miss_rate",
                           "Last level cache misses per last level cache access",
                           d_ratio(llc_miss_rd, llc_access), "100%")
                ]) if llc_miss_rd and llc_access else None,
            ]),
        ]),
    ])


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
        Arm64Topdown(),
    ])

    if _args.metricgroups:
        print(JsonEncodeMetricGroupDescriptions(all_metrics))
    else:
        print(JsonEncodeMetric(all_metrics))


if __name__ == '__main__':
    main()
