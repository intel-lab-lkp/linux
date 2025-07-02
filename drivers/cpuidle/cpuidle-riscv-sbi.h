/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CPUIDLE_RISCV_SBI
#define __CPUIDLE_RISCV_SBI

#ifdef CONFIG_DT_IDLE_GENPD

int sbi_cpuidle_pd_power_off(struct generic_pm_domain *pd);

#else

static inline int sbi_cpuidle_pd_power_off(struct generic_pm_domain *pd)
{
	return 0;
}

#endif

int sbi_dt_parse_state_node(struct device_node *np, u32 *state);

#endif
