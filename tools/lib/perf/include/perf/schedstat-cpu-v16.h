/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count, v16)
CPU_FIELD(__u32, array_exp, v16)
CPU_FIELD(__u32, sched_count, v16)
CPU_FIELD(__u32, sched_goidle, v16)
CPU_FIELD(__u32, ttwu_count, v16)
CPU_FIELD(__u32, ttwu_local, v16)
CPU_FIELD(__u64, rq_cpu_time, v16)
CPU_FIELD(__u64, run_delay, v16)
CPU_FIELD(__u64, pcount, v16)
#endif /* CPU_FIELD */
