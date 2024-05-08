/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count)
CPU_FIELD(__u32, array_exp)
CPU_FIELD(__u32, sched_count)
CPU_FIELD(__u32, sched_goidle)
CPU_FIELD(__u32, ttwu_count)
CPU_FIELD(__u32, ttwu_local)
CPU_FIELD(__u64, rq_cpu_time)
CPU_FIELD(__u64, run_delay)
CPU_FIELD(__u64, pcount)
#endif
