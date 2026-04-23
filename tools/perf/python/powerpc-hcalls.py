#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""
Hypervisor call statistics

Copyright (C) 2018 Ravi Bangoria, IBM Corporation
Ported from tools/perf/scripts/python/powerpc-hcalls.py
"""

import argparse
from collections import defaultdict
import perf

# Hypervisor call table
HCALL_TABLE = {
    4: 'H_REMOVE',
    8: 'H_ENTER',
    12: 'H_READ',
    16: 'H_CLEAR_MOD',
    20: 'H_CLEAR_REF',
    24: 'H_PROTECT',
    28: 'H_GET_TCE',
    32: 'H_PUT_TCE',
    36: 'H_SET_SPRG0',
    40: 'H_SET_DABR',
    44: 'H_PAGE_INIT',
    48: 'H_SET_ASR',
    52: 'H_ASR_ON',
    56: 'H_ASR_OFF',
    60: 'H_LOGICAL_CI_LOAD',
    64: 'H_LOGICAL_CI_STORE',
    68: 'H_LOGICAL_CACHE_LOAD',
    72: 'H_LOGICAL_CACHE_STORE',
    76: 'H_LOGICAL_ICBI',
    80: 'H_LOGICAL_DCBF',
    84: 'H_GET_TERM_CHAR',
    88: 'H_PUT_TERM_CHAR',
    92: 'H_REAL_TO_LOGICAL',
    96: 'H_HYPERVISOR_DATA',
    100: 'H_EOI',
    104: 'H_CPPR',
    108: 'H_IPI',
    112: 'H_IPOLL',
    116: 'H_XIRR',
    120: 'H_MIGRATE_DMA',
    124: 'H_PERFMON',
    220: 'H_REGISTER_VPA',
    224: 'H_CEDE',
    228: 'H_CONFER',
    232: 'H_PROD',
    236: 'H_GET_PPP',
    240: 'H_SET_PPP',
    244: 'H_PURR',
    248: 'H_PIC',
    252: 'H_REG_CRQ',
    256: 'H_FREE_CRQ',
    260: 'H_VIO_SIGNAL',
    264: 'H_SEND_CRQ',
    272: 'H_COPY_RDMA',
    276: 'H_REGISTER_LOGICAL_LAN',
    280: 'H_FREE_LOGICAL_LAN',
    284: 'H_ADD_LOGICAL_LAN_BUFFER',
    288: 'H_SEND_LOGICAL_LAN',
    292: 'H_BULK_REMOVE',
    304: 'H_MULTICAST_CTRL',
    308: 'H_SET_XDABR',
    312: 'H_STUFF_TCE',
    316: 'H_PUT_TCE_INDIRECT',
    332: 'H_CHANGE_LOGICAL_LAN_MAC',
    336: 'H_VTERM_PARTNER_INFO',
    340: 'H_REGISTER_VTERM',
    344: 'H_FREE_VTERM',
    348: 'H_RESET_EVENTS',
    352: 'H_ALLOC_RESOURCE',
    356: 'H_FREE_RESOURCE',
    360: 'H_MODIFY_QP',
    364: 'H_QUERY_QP',
    368: 'H_REREGISTER_PMR',
    372: 'H_REGISTER_SMR',
    376: 'H_QUERY_MR',
    380: 'H_QUERY_MW',
    384: 'H_QUERY_HCA',
    388: 'H_QUERY_PORT',
    392: 'H_MODIFY_PORT',
    396: 'H_DEFINE_AQP1',
    400: 'H_GET_TRACE_BUFFER',
    404: 'H_DEFINE_AQP0',
    408: 'H_RESIZE_MR',
    412: 'H_ATTACH_MCQP',
    416: 'H_DETACH_MCQP',
    420: 'H_CREATE_RPT',
    424: 'H_REMOVE_RPT',
    428: 'H_REGISTER_RPAGES',
    432: 'H_DISABLE_AND_GETC',
    436: 'H_ERROR_DATA',
    440: 'H_GET_HCA_INFO',
    444: 'H_GET_PERF_COUNT',
    448: 'H_MANAGE_TRACE',
    468: 'H_FREE_LOGICAL_LAN_BUFFER',
    472: 'H_POLL_PENDING',
    484: 'H_QUERY_INT_STATE',
    580: 'H_ILLAN_ATTRIBUTES',
    592: 'H_MODIFY_HEA_QP',
    596: 'H_QUERY_HEA_QP',
    600: 'H_QUERY_HEA',
    604: 'H_QUERY_HEA_PORT',
    608: 'H_MODIFY_HEA_PORT',
    612: 'H_REG_BCMC',
    616: 'H_DEREG_BCMC',
    620: 'H_REGISTER_HEA_RPAGES',
    624: 'H_DISABLE_AND_GET_HEA',
    628: 'H_GET_HEA_INFO',
    632: 'H_ALLOC_HEA_RESOURCE',
    644: 'H_ADD_CONN',
    648: 'H_DEL_CONN',
    664: 'H_JOIN',
    676: 'H_VASI_STATE',
    688: 'H_ENABLE_CRQ',
    696: 'H_GET_EM_PARMS',
    720: 'H_SET_MPP',
    724: 'H_GET_MPP',
    748: 'H_HOME_NODE_ASSOCIATIVITY',
    756: 'H_BEST_ENERGY',
    764: 'H_XIRR_X',
    768: 'H_RANDOM',
    772: 'H_COP',
    788: 'H_GET_MPP_X',
    796: 'H_SET_MODE',
    61440: 'H_RTAS',
}


class HCallAnalyzer:
    """Analyzes hypervisor calls and aggregates statistics."""

    def __init__(self):
        # output: {opcode: {'min': min, 'max': max, 'time': time, 'cnt': cnt}}
        self.output = defaultdict(lambda: {'time': 0, 'cnt': 0, 'min': float('inf'), 'max': 0})
        # d_enter: {pid: (opcode, nsec)}
        self.d_enter: dict[int, tuple[int, int]] = {}
        self.print_ptrn = '%-28s%10s%10s%10s%10s'

    def hcall_table_lookup(self, opcode: int) -> str:
        """Lookup hcall name by opcode."""
        return HCALL_TABLE.get(opcode, str(opcode))

    def process_event(self, sample: perf.sample_event) -> None:
        """Process a single sample event."""
        name = str(sample.evsel)
        pid = sample.sample_pid
        time = sample.time
        opcode = getattr(sample, "opcode", -1)

        if opcode == -1:
            return

        if name == "evsel(powerpc:hcall_entry)":
            self.d_enter[pid] = (opcode, time)
        elif name == "evsel(powerpc:hcall_exit)":
            if pid in self.d_enter:
                opcode_entry, time_entry = self.d_enter[pid]
                if opcode_entry == opcode:
                    diff = time - time_entry
                    del self.d_enter[pid]

                    stats = self.output[opcode]
                    stats['time'] += diff
                    stats['cnt'] += 1
                    if diff < stats['min']:
                        stats['min'] = diff
                    if diff > stats['max']:
                        stats['max'] = diff

    def print_summary(self) -> None:
        """Print aggregated statistics."""
        print(self.print_ptrn % ('hcall', 'count', 'min(ns)', 'max(ns)', 'avg(ns)'))
        print('-' * 68)
        for opcode in sorted(self.output.keys()):
            h_name = self.hcall_table_lookup(opcode)
            stats = self.output[opcode]
            time = stats['time']
            cnt = stats['cnt']
            min_t = stats['min']
            max_t = stats['max']

            # Avoid float representation for large integers if possible,
            # or use formatted strings. Legacy used time//cnt.
            avg_t = time // cnt if cnt > 0 else 0

            # If min was not updated, it remains inf, but cnt should be > 0 if in output
            if min_t == float('inf'):
                min_t = 0

            print(self.print_ptrn % (h_name, cnt, int(min_t), int(max_t), avg_t))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Hypervisor call statistics")
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()

    analyzer = HCallAnalyzer()

    try:
        session = perf.session(perf.data(args.input), sample=analyzer.process_event)
        session.process_events()
        analyzer.print_summary()
    except KeyboardInterrupt:
        analyzer.print_summary()
    except Exception as e:
        print(f"Error processing events: {e}")
