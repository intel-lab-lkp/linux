/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count, v15)
CPU_FIELD(__u32, array_exp, v15)
CPU_FIELD(__u32, sched_count, v15)
CPU_FIELD(__u32, sched_goidle, v15)
CPU_FIELD(__u32, ttwu_count, v15)
CPU_FIELD(__u32, ttwu_local, v15)
CPU_FIELD(__u64, rq_cpu_time, v15)
CPU_FIELD(__u64, run_delay, v15)
CPU_FIELD(__u64, pcount, v15)
#endif
