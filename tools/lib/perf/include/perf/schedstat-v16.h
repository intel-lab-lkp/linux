/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CPU_FIELD
CPU_FIELD(__u32, yld_count, v16);
CPU_FIELD(__u32, array_exp, v16);
CPU_FIELD(__u32, sched_count, v16);
CPU_FIELD(__u32, sched_goidle, v16);
CPU_FIELD(__u32, ttwu_count, v16);
CPU_FIELD(__u32, ttwu_local, v16);
CPU_FIELD(__u64, rq_cpu_time, v16);
CPU_FIELD(__u64, run_delay, v16);
CPU_FIELD(__u64, pcount, v16);
#endif

#ifdef DOMAIN_FIELD
DOMAIN_FIELD(__u32, busy_lb_count, v16);
DOMAIN_FIELD(__u32, busy_lb_balanced, v16);
DOMAIN_FIELD(__u32, busy_lb_failed, v16);
DOMAIN_FIELD(__u32, busy_lb_imbalance, v16);
DOMAIN_FIELD(__u32, busy_lb_gained, v16);
DOMAIN_FIELD(__u32, busy_lb_hot_gained, v16);
DOMAIN_FIELD(__u32, busy_lb_nobusyq, v16);
DOMAIN_FIELD(__u32, busy_lb_nobusyg, v16);
DOMAIN_FIELD(__u32, idle_lb_count, v16);
DOMAIN_FIELD(__u32, idle_lb_balanced, v16);
DOMAIN_FIELD(__u32, idle_lb_failed, v16);
DOMAIN_FIELD(__u32, idle_lb_imbalance, v16);
DOMAIN_FIELD(__u32, idle_lb_gained, v16);
DOMAIN_FIELD(__u32, idle_lb_hot_gained, v16);
DOMAIN_FIELD(__u32, idle_lb_nobusyq, v16);
DOMAIN_FIELD(__u32, idle_lb_nobusyg, v16);
DOMAIN_FIELD(__u32, newidle_lb_count, v16);
DOMAIN_FIELD(__u32, newidle_lb_balanced, v16);
DOMAIN_FIELD(__u32, newidle_lb_failed, v16);
DOMAIN_FIELD(__u32, newidle_lb_imbalance, v16);
DOMAIN_FIELD(__u32, newidle_lb_gained, v16);
DOMAIN_FIELD(__u32, newidle_lb_hot_gained, v16);
DOMAIN_FIELD(__u32, newidle_lb_nobusyq, v16);
DOMAIN_FIELD(__u32, newidle_lb_nobusyg, v16);
DOMAIN_FIELD(__u32, alb_count, v16);
DOMAIN_FIELD(__u32, alb_failed, v16);
DOMAIN_FIELD(__u32, alb_pushed, v16);
DOMAIN_FIELD(__u32, sbe_count, v16);
DOMAIN_FIELD(__u32, sbe_balanced, v16);
DOMAIN_FIELD(__u32, sbe_pushed, v16);
DOMAIN_FIELD(__u32, sbf_count, v16);
DOMAIN_FIELD(__u32, sbf_balanced, v16);
DOMAIN_FIELD(__u32, sbf_pushed, v16);
DOMAIN_FIELD(__u32, ttwu_wake_remote, v16);
DOMAIN_FIELD(__u32, ttwu_move_affine, v16);
DOMAIN_FIELD(__u32, ttwu_move_balance, v16);
#endif
