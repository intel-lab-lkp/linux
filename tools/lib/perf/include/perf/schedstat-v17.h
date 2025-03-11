/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count, v17);
CPU_FIELD(__u32, array_exp, v17);
CPU_FIELD(__u32, sched_count, v17);
CPU_FIELD(__u32, sched_goidle, v17);
CPU_FIELD(__u32, ttwu_count, v17);
CPU_FIELD(__u32, ttwu_local, v17);
CPU_FIELD(__u64, rq_cpu_time, v17);
CPU_FIELD(__u64, run_delay, v17);
CPU_FIELD(__u64, pcount, v17);
#endif

#ifdef DOMAIN_FIELD
DOMAIN_FIELD(__u32, busy_lb_count, v17);
DOMAIN_FIELD(__u32, busy_lb_balanced, v17);
DOMAIN_FIELD(__u32, busy_lb_failed, v17);
DOMAIN_FIELD(__u32, busy_lb_imbalance_load, v17);
DOMAIN_FIELD(__u32, busy_lb_imbalance_util, v17);
DOMAIN_FIELD(__u32, busy_lb_imbalance_task, v17);
DOMAIN_FIELD(__u32, busy_lb_imbalance_misfit, v17);
DOMAIN_FIELD(__u32, busy_lb_gained, v17);
DOMAIN_FIELD(__u32, busy_lb_hot_gained, v17);
DOMAIN_FIELD(__u32, busy_lb_nobusyq, v17);
DOMAIN_FIELD(__u32, busy_lb_nobusyg, v17);
DOMAIN_FIELD(__u32, idle_lb_count, v17);
DOMAIN_FIELD(__u32, idle_lb_balanced, v17);
DOMAIN_FIELD(__u32, idle_lb_failed, v17);
DOMAIN_FIELD(__u32, idle_lb_imbalance_load, v17);
DOMAIN_FIELD(__u32, idle_lb_imbalance_util, v17);
DOMAIN_FIELD(__u32, idle_lb_imbalance_task, v17);
DOMAIN_FIELD(__u32, idle_lb_imbalance_misfit, v17);
DOMAIN_FIELD(__u32, idle_lb_gained, v17);
DOMAIN_FIELD(__u32, idle_lb_hot_gained, v17);
DOMAIN_FIELD(__u32, idle_lb_nobusyq, v17);
DOMAIN_FIELD(__u32, idle_lb_nobusyg, v17);
DOMAIN_FIELD(__u32, newidle_lb_count, v17);
DOMAIN_FIELD(__u32, newidle_lb_balanced, v17);
DOMAIN_FIELD(__u32, newidle_lb_failed, v17);
DOMAIN_FIELD(__u32, newidle_lb_imbalance_load, v17);
DOMAIN_FIELD(__u32, newidle_lb_imbalance_util, v17);
DOMAIN_FIELD(__u32, newidle_lb_imbalance_task, v17);
DOMAIN_FIELD(__u32, newidle_lb_imbalance_misfit, v17);
DOMAIN_FIELD(__u32, newidle_lb_gained, v17);
DOMAIN_FIELD(__u32, newidle_lb_hot_gained, v17);
DOMAIN_FIELD(__u32, newidle_lb_nobusyq, v17);
DOMAIN_FIELD(__u32, newidle_lb_nobusyg, v17);
DOMAIN_FIELD(__u32, alb_count, v17);
DOMAIN_FIELD(__u32, alb_failed, v17);
DOMAIN_FIELD(__u32, alb_pushed, v17);
DOMAIN_FIELD(__u32, sbe_count, v17);
DOMAIN_FIELD(__u32, sbe_balanced, v17);
DOMAIN_FIELD(__u32, sbe_pushed, v17);
DOMAIN_FIELD(__u32, sbf_count, v17);
DOMAIN_FIELD(__u32, sbf_balanced, v17);
DOMAIN_FIELD(__u32, sbf_pushed, v17);
DOMAIN_FIELD(__u32, ttwu_wake_remote, v17);
DOMAIN_FIELD(__u32, ttwu_move_affine, v17);
DOMAIN_FIELD(__u32, ttwu_move_balance, v17);
#endif
