/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count, v15);
CPU_FIELD(__u32, array_exp, v15);
CPU_FIELD(__u32, sched_count, v15);
CPU_FIELD(__u32, sched_goidle, v15);
CPU_FIELD(__u32, ttwu_count, v15);
CPU_FIELD(__u32, ttwu_local, v15);
CPU_FIELD(__u64, rq_cpu_time, v15);
CPU_FIELD(__u64, run_delay, v15);
CPU_FIELD(__u64, pcount, v15);
#endif

#ifdef DOMAIN_FIELD
DOMAIN_FIELD(__u32, idle_lb_count, v15);
DOMAIN_FIELD(__u32, idle_lb_balanced, v15);
DOMAIN_FIELD(__u32, idle_lb_failed, v15);
DOMAIN_FIELD(__u32, idle_lb_imbalance, v15);
DOMAIN_FIELD(__u32, idle_lb_gained, v15);
DOMAIN_FIELD(__u32, idle_lb_hot_gained, v15);
DOMAIN_FIELD(__u32, idle_lb_nobusyq, v15);
DOMAIN_FIELD(__u32, idle_lb_nobusyg, v15);
DOMAIN_FIELD(__u32, busy_lb_count, v15);
DOMAIN_FIELD(__u32, busy_lb_balanced, v15);
DOMAIN_FIELD(__u32, busy_lb_failed, v15);
DOMAIN_FIELD(__u32, busy_lb_imbalance, v15);
DOMAIN_FIELD(__u32, busy_lb_gained, v15);
DOMAIN_FIELD(__u32, busy_lb_hot_gained, v15);
DOMAIN_FIELD(__u32, busy_lb_nobusyq, v15);
DOMAIN_FIELD(__u32, busy_lb_nobusyg, v15);
DOMAIN_FIELD(__u32, newidle_lb_count, v15);
DOMAIN_FIELD(__u32, newidle_lb_balanced, v15);
DOMAIN_FIELD(__u32, newidle_lb_failed, v15);
DOMAIN_FIELD(__u32, newidle_lb_imbalance, v15);
DOMAIN_FIELD(__u32, newidle_lb_gained, v15);
DOMAIN_FIELD(__u32, newidle_lb_hot_gained, v15);
DOMAIN_FIELD(__u32, newidle_lb_nobusyq, v15);
DOMAIN_FIELD(__u32, newidle_lb_nobusyg, v15);
DOMAIN_FIELD(__u32, alb_count, v15);
DOMAIN_FIELD(__u32, alb_failed, v15);
DOMAIN_FIELD(__u32, alb_pushed, v15);
DOMAIN_FIELD(__u32, sbe_count, v15);
DOMAIN_FIELD(__u32, sbe_balanced, v15);
DOMAIN_FIELD(__u32, sbe_pushed, v15);
DOMAIN_FIELD(__u32, sbf_count, v15);
DOMAIN_FIELD(__u32, sbf_balanced, v15);
DOMAIN_FIELD(__u32, sbf_pushed, v15);
DOMAIN_FIELD(__u32, ttwu_wake_remote, v15);
DOMAIN_FIELD(__u32, ttwu_move_affine, v15);
DOMAIN_FIELD(__u32, ttwu_move_balance, v15);
#endif
