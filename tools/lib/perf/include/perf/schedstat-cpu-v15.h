/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count, "sched_yield() count",
	  "%11u", false, yld_count)
CPU_FIELD(__u32, array_exp, "Legacy counter can be ignored",
	  "%11u", false, array_exp)
CPU_FIELD(__u32, sched_count, "schedule() called",
	  "%11u", false, sched_count)
CPU_FIELD(__u32, sched_goidle, "schedule() left the processor idle",
	  "%11u", true, sched_count)
CPU_FIELD(__u32, ttwu_count, "try_to_wake_up() was called",
	  "%11u", false, ttwu_count)
CPU_FIELD(__u32, ttwu_local, "try_to_wake_up() was called to wake up the local cpu",
	  "%11u", true, ttwu_count)
CPU_FIELD(__u64, rq_cpu_time, "total runtime by tasks on this processor (in jiffies)",
	  "%11llu", false, rq_cpu_time)
CPU_FIELD(__u64, run_delay, "total waittime by tasks on this processor (in jiffies)",
	  "%11llu", true, rq_cpu_time)
CPU_FIELD(__u64, pcount, "total timeslices run on this cpu",
	  "%11llu", false, pcount)
#endif /* CPU_FIELD */
