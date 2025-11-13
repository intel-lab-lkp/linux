#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
import argparse
import math
import os
from typing import Optional
from metric import (d_ratio, has_event, max, Event, JsonEncodeMetric,
                    JsonEncodeMetricGroupDescriptions, Literal, LoadEvents,
                    Metric, MetricGroup, Select)

# Global command line arguments.
_args = None
_zen_model: int = 1
interval_sec = Event("duration_time")
ins = Event("instructions")
cycles = Event("cycles")
# Number of CPU cycles scaled for SMT.
smt_cycles = Select(cycles / 2, Literal("#smt_on"), cycles)


def AmdBr():
    def Total() -> MetricGroup:
        br = Event("ex_ret_brn")
        br_m_all = Event("ex_ret_brn_misp")
        br_clr = Event("ex_ret_msprd_brnch_instr_dir_msmtch",
                       "ex_ret_brn_resync")

        br_r = d_ratio(br, interval_sec)
        ins_r = d_ratio(ins, br)
        misp_r = d_ratio(br_m_all, br)
        clr_r = d_ratio(br_clr, interval_sec)

        return MetricGroup("lpm_br_total", [
            Metric("lpm_br_total_retired",
                   "The number of branch instructions retired per second.", br_r,
                   "insn/s"),
            Metric(
                "lpm_br_total_mispred",
                "The number of branch instructions retired, of any type, that were "
                "not correctly predicted as a percentage of all branch instrucions.",
                misp_r, "100%"),
            Metric("lpm_br_total_insn_between_branches",
                   "The number of instructions divided by the number of branches.",
                   ins_r, "insn"),
            Metric("lpm_br_total_insn_fe_resteers",
                   "The number of resync branches per second.", clr_r, "req/s")
        ])

    def Taken() -> MetricGroup:
        br = Event("ex_ret_brn_tkn")
        br_m_tk = Event("ex_ret_brn_tkn_misp")
        br_r = d_ratio(br, interval_sec)
        ins_r = d_ratio(ins, br)
        misp_r = d_ratio(br_m_tk, br)
        return MetricGroup("lpm_br_taken", [
            Metric("lpm_br_taken_retired",
                   "The number of taken branches that were retired per second.",
                   br_r, "insn/s"),
            Metric(
                "lpm_br_taken_mispred",
                "The number of retired taken branch instructions that were "
                "mispredicted as a percentage of all taken branches.", misp_r,
                "100%"),
            Metric(
                "lpm_br_taken_insn_between_branches",
                "The number of instructions divided by the number of taken branches.",
                ins_r, "insn"),
        ])

    def Conditional() -> Optional[MetricGroup]:
        global _zen_model
        br = Event("ex_ret_cond")
        br_r = d_ratio(br, interval_sec)
        ins_r = d_ratio(ins, br)

        metrics = [
            Metric("lpm_br_cond_retired", "Retired conditional branch instructions.",
                   br_r, "insn/s"),
            Metric("lpm_br_cond_insn_between_branches",
                   "The number of instructions divided by the number of conditional "
                   "branches.", ins_r, "insn"),
        ]
        if _zen_model == 2:
            br_m_cond = Event("ex_ret_cond_misp")
            misp_r = d_ratio(br_m_cond, br)
            metrics += [
                Metric("lpm_br_cond_mispred",
                       "Retired conditional branch instructions mispredicted as a "
                       "percentage of all conditional branches.", misp_r, "100%"),
            ]

        return MetricGroup("lpm_br_cond", metrics)

    def Fused() -> MetricGroup:
        br = Event("ex_ret_fused_instr", "ex_ret_fus_brnch_inst")
        br_r = d_ratio(br, interval_sec)
        ins_r = d_ratio(ins, br)
        return MetricGroup("lpm_br_cond", [
            Metric("lpm_br_fused_retired",
                   "Retired fused branch instructions per second.", br_r, "insn/s"),
            Metric(
                "lpm_br_fused_insn_between_branches",
                "The number of instructions divided by the number of fused "
                "branches.", ins_r, "insn"),
        ])

    def Far() -> MetricGroup:
        br = Event("ex_ret_brn_far")
        br_r = d_ratio(br, interval_sec)
        ins_r = d_ratio(ins, br)
        return MetricGroup("lpm_br_far", [
            Metric("lpm_br_far_retired", "Retired far control transfers per second.",
                   br_r, "insn/s"),
            Metric(
                "lpm_br_far_insn_between_branches",
                "The number of instructions divided by the number of far branches.",
                ins_r, "insn"),
        ])

    return MetricGroup("lpm_br", [Total(), Taken(), Conditional(), Fused(), Far()],
                       description="breakdown of retired branch instructions")


def AmdHwpf():
    """Returns a MetricGroup representing AMD hardware prefetch metrics."""
    global _zen_model
    if _zen_model <= 1:
        return None

    hwp_ld = Event("ls_dispatch.ld_dispatch")
    hwp_l2 = Event("ls_hw_pf_dc_fills.local_l2",
                   "ls_hw_pf_dc_fills.lcl_l2",
                   "ls_hw_pf_dc_fill.ls_mabresp_lcl_l2")
    hwp_lc = Event("ls_hw_pf_dc_fills.local_ccx",
                   "ls_hw_pf_dc_fills.int_cache",
                   "ls_hw_pf_dc_fill.ls_mabresp_lcl_cache")
    hwp_lm = Event("ls_hw_pf_dc_fills.dram_io_near",
                   "ls_hw_pf_dc_fills.mem_io_local",
                   "ls_hw_pf_dc_fill.ls_mabresp_lcl_dram")
    hwp_rc = Event("ls_hw_pf_dc_fills.far_cache",
                   "ls_hw_pf_dc_fills.ext_cache_remote",
                   "ls_hw_pf_dc_fill.ls_mabresp_rmt_cache")
    hwp_rm = Event("ls_hw_pf_dc_fills.dram_io_far",
                   "ls_hw_pf_dc_fills.mem_io_remote",
                   "ls_hw_pf_dc_fill.ls_mabresp_rmt_dram")

    loc_pf = hwp_l2 + hwp_lc + hwp_lm
    rem_pf = hwp_rc + hwp_rm
    all_pf = loc_pf + rem_pf

    r1 = d_ratio(ins, all_pf)
    r2 = d_ratio(hwp_ld, all_pf)
    r3 = d_ratio(all_pf, interval_sec)

    overview = MetricGroup("lpm_hwpf_overview", [
        Metric("lpm_hwpf_ov_insn_bt_hwpf", "Insn between HWPF", r1, "insns"),
        Metric("lpm_hwpf_ov_loads_bt_hwpf", "Loads between HWPF", r2, "loads"),
        Metric("lpm_hwpf_ov_rate", "HWPF per second", r3, "hwpf/s"),
    ])
    r1 = d_ratio(hwp_l2, all_pf)
    r2 = d_ratio(hwp_lc, all_pf)
    r3 = d_ratio(hwp_lm, all_pf)
    data_src_local = MetricGroup("lpm_hwpf_data_src_local", [
        Metric("lpm_hwpf_data_src_local_l2",
               "Data source local l2", r1, "100%"),
        Metric("lpm_hwpf_data_src_local_ccx_l3_loc_ccx",
               "Data source local ccx l3 loc ccx", r2, "100%"),
        Metric("lpm_hwpf_data_src_local_memory_or_io",
               "Data source local memory or IO", r3, "100%"),
    ])

    r1 = d_ratio(hwp_rc, all_pf)
    r2 = d_ratio(hwp_rm, all_pf)
    data_src_remote = MetricGroup("lpm_hwpf_data_src_remote", [
        Metric("lpm_hwpf_data_src_remote_cache", "Data source remote cache", r1,
               "100%"),
        Metric("lpm_hwpf_data_src_remote_memory_or_io",
               "Data source remote memory or IO", r2, "100%"),
    ])

    data_src = MetricGroup("lpm_hwpf_data_src", [
                           data_src_local, data_src_remote])
    return MetricGroup("lpm_hwpf", [overview, data_src],
                       description="Hardware prefetch breakdown (CCX L3 = L3 of current thread, Loc CCX = CCX cache on some socket)")


def AmdSwpf() -> Optional[MetricGroup]:
    """Returns a MetricGroup representing AMD software prefetch metrics."""
    global _zen_model
    if _zen_model <= 1:
        return None

    swp_ld = Event("ls_dispatch.ld_dispatch")
    swp_t0 = Event("ls_pref_instr_disp.prefetch")
    swp_w = Event("ls_pref_instr_disp.prefetch_w")  # Missing on Zen1
    swp_nt = Event("ls_pref_instr_disp.prefetch_nta")
    swp_mab = Event("ls_inef_sw_pref.mab_mch_cnt")
    swp_l2 = Event("ls_sw_pf_dc_fills.local_l2",
                   "ls_sw_pf_dc_fills.lcl_l2",
                   "ls_sw_pf_dc_fill.ls_mabresp_lcl_l2")
    swp_lc = Event("ls_sw_pf_dc_fills.local_ccx",
                   "ls_sw_pf_dc_fills.int_cache",
                   "ls_sw_pf_dc_fill.ls_mabresp_lcl_cache")
    swp_lm = Event("ls_sw_pf_dc_fills.dram_io_near",
                   "ls_sw_pf_dc_fills.mem_io_local",
                   "ls_sw_pf_dc_fill.ls_mabresp_lcl_dram")
    swp_rc = Event("ls_sw_pf_dc_fills.far_cache",
                   "ls_sw_pf_dc_fills.ext_cache_remote",
                   "ls_sw_pf_dc_fill.ls_mabresp_rmt_cache")
    swp_rm = Event("ls_sw_pf_dc_fills.dram_io_far",
                   "ls_sw_pf_dc_fills.mem_io_remote",
                   "ls_sw_pf_dc_fill.ls_mabresp_rmt_dram")

    # All the swpf that were satisfied beyond L1D are good.
    all_pf = swp_t0 + swp_w + swp_nt
    good_pf = swp_l2 + swp_lc + swp_lm + swp_rc + swp_rm
    bad_pf = max(all_pf - good_pf, 0)

    loc_pf = swp_l2 + swp_lc + swp_lm
    rem_pf = swp_rc + swp_rm

    req_pend = max(0, bad_pf - swp_mab)

    r1 = d_ratio(ins, all_pf)
    r2 = d_ratio(swp_ld, all_pf)
    r3 = d_ratio(swp_t0, interval_sec)
    r4 = d_ratio(swp_w, interval_sec)
    r5 = d_ratio(swp_nt, interval_sec)
    overview = MetricGroup("lpm_swpf_overview", [
        Metric("lpm_swpf_ov_insn_bt_swpf", "Insn between SWPF", r1, "insns"),
        Metric("lpm_swpf_ov_loads_bt_swpf", "Loads between SWPF", r2, "loads"),
        Metric("lpm_swpf_ov_rate_prefetch_t0_t1_t2", "Rate prefetch TO_T1_T2", r3,
               "insns/sec"),
        Metric("lpm_swpf_ov_rate_prefetch_w",
               "Rate prefetch W", r4, "insns/sec"),
        Metric("lpm_swpf_ov_rate_preftech_nta",
               "Rate prefetch NTA", r5, "insns/sec"),
    ])

    r1 = d_ratio(swp_mab, all_pf)
    r2 = d_ratio(req_pend, all_pf)
    usefulness_bad = MetricGroup("lpm_swpf_usefulness_bad", [
        Metric("lpm_swpf_use_bad_hit_l1", "Usefulness bad hit L1", r1, "100%"),
        Metric("lpm_swpf_use_bad_req_pend",
               "Usefulness bad req pending", r2, "100%"),
    ])

    r1 = d_ratio(good_pf, all_pf)
    usefulness_good = MetricGroup("lpm_swpf_usefulness_good", [
        Metric("lpm_swpf_use_good_other_src", "Usefulness good other src", r1,
               "100%"),
    ])

    usefulness = MetricGroup("lpm_swpf_usefulness", [
        usefulness_bad,
        usefulness_good,
    ])

    r1 = d_ratio(swp_l2, good_pf)
    r2 = d_ratio(swp_lc, good_pf)
    r3 = d_ratio(swp_lm, good_pf)
    data_src_local = MetricGroup("lpm_swpf_data_src_local", [
        Metric("lpm_swpf_data_src_local_l2",
               "Data source local l2", r1, "100%"),
        Metric("lpm_swpf_data_src_local_ccx_l3_loc_ccx",
               "Data source local ccx l3 loc ccx", r2, "100%"),
        Metric("lpm_swpf_data_src_local_memory_or_io",
               "Data source local memory or IO", r3, "100%"),
    ])

    r1 = d_ratio(swp_rc, good_pf)
    r2 = d_ratio(swp_rm, good_pf)
    data_src_remote = MetricGroup("lpm_swpf_data_src_remote", [
        Metric("lpm_swpf_data_src_remote_cache", "Data source remote cache", r1,
               "100%"),
        Metric("lpm_swpf_data_src_remote_memory_or_io",
               "Data source remote memory or IO", r2, "100%"),
    ])

    data_src = MetricGroup("lpm_swpf_data_src", [
                           data_src_local, data_src_remote])

    return MetricGroup("lpm_swpf", [overview, usefulness, data_src],
                       description="Software prefetch breakdown (CCX L3 = L3 of current thread, Loc CCX = CCX cache on some socket)")


def AmdUpc() -> Metric:
    ops = Event("ex_ret_ops", "ex_ret_cops")
    upc = d_ratio(ops, smt_cycles)
    return Metric("lpm_upc", "Micro-ops retired per core cycle (higher is better)",
                  upc, "uops/cycle")


def Idle() -> Metric:
    cyc = Event("msr/mperf/")
    tsc = Event("msr/tsc/")
    low = max(tsc - cyc, 0)
    return Metric(
        "lpm_idle",
        "Percentage of total wallclock cycles where CPUs are in low power state (C1 or deeper sleep state)",
        d_ratio(low, tsc), "100%")


def Rapl() -> MetricGroup:
    """Processor socket power consumption estimate.

    Use events from the running average power limit (RAPL) driver.
    """
    # Watts = joules/second
    # Currently only energy-pkg is supported by AMD:
    # https://lore.kernel.org/lkml/20220105185659.643355-1-eranian@google.com/
    pkg = Event("power/energy\\-pkg/")
    cond_pkg = Select(pkg, has_event(pkg), math.nan)
    scale = 2.3283064365386962890625e-10
    metrics = [
        Metric("lpm_cpu_power_pkg", "",
               d_ratio(cond_pkg * scale, interval_sec), "Watts"),
    ]

    return MetricGroup("lpm_cpu_power", metrics,
                       description="Processor socket power consumption estimates")


def main() -> None:
    global _args
    global _zen_model

    def dir_path(path: str) -> str:
        """Validate path is a directory for argparse."""
        if os.path.isdir(path):
            return path
        raise argparse.ArgumentTypeError(
            f'\'{path}\' is not a valid directory')

    parser = argparse.ArgumentParser(description="AMD perf json generator")
    parser.add_argument(
        "-metricgroups", help="Generate metricgroups data", action='store_true')
    parser.add_argument("model", help="e.g. amdzen[123]")
    parser.add_argument(
        'events_path',
        type=dir_path,
        help='Root of tree containing architecture directories containing json files'
    )
    _args = parser.parse_args()

    directory = f"{_args.events_path}/x86/{_args.model}/"
    LoadEvents(directory)

    _zen_model = int(_args.model[6:])

    all_metrics = MetricGroup("", [
        AmdBr(),
        AmdHwpf(),
        AmdSwpf(),
        AmdUpc(),
        Idle(),
        Rapl(),
    ])

    if _args.metricgroups:
        print(JsonEncodeMetricGroupDescriptions(all_metrics))
    else:
        print(JsonEncodeMetric(all_metrics))


if __name__ == '__main__':
    main()
