/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CPUIDLE_RISCV_SBI_H
#define __CPUIDLE_RISCV_SBI_H

#ifdef CONFIG_RISCV_SBI_CPUIDLE

void sbi_set_osi_mode(bool use_osi);
void sbi_set_domain_state(u32 state);
int sbi_dt_parse_state_node(struct device_node *np, u32 *state);

#else

static inline void sbi_set_osi_mode(bool use_osi)
{
}

static inline void sbi_set_domain_state(u32 state)
{
}

static inline int sbi_dt_parse_state_node(struct device_node *np, u32 *state)
{
	return 0;
}

#endif

#endif /* __CPUIDLE_RISCV_SBI_H */
