/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RISC-V RPMI performance service group interface
 *
 * Copyright (C) 2026 Shanghai StarFive Technology Co., Ltd.
 *
 * The RPMI performance service group is a provider of performance domains.
 * The front-ends that drive a domain reach the platform microcontroller
 * through this interface, so that the mailbox channel and the domain
 * enumeration have a single owner.
 */

#ifndef _LINUX_RISCV_RPMI_PERFORMANCE_H_
#define _LINUX_RISCV_RPMI_PERFORMANCE_H_

#include <linux/types.h>

struct device;
struct rpmi_perf;
struct rpmi_perf_domain;

/**
 * struct rpmi_perf_level - one level of a performance domain
 *
 * @index: level index, which is what the RPMI services take and return
 * @clock_freq: clock frequency of the level, in kHz
 * @power_cost: power cost of the level, in uW
 * @trans_latency_us: worst case time to switch to this level, in us
 */
struct rpmi_perf_level {
	u32 index;
	u32 clock_freq;
	u32 power_cost;
	u32 trans_latency_us;
};

#if IS_ENABLED(CONFIG_RISCV_RPMI_PERFORMANCE)

struct rpmi_perf_domain *rpmi_perf_domain_by_id(struct rpmi_perf *perf, u32 id);
u32 rpmi_perf_num_domains(struct rpmi_perf *perf);

const char *rpmi_perf_domain_name(struct rpmi_perf_domain *pd);
u32 rpmi_perf_domain_level_count(struct rpmi_perf_domain *pd);
int rpmi_perf_domain_level_info(struct rpmi_perf_domain *pd, u32 idx,
				struct rpmi_perf_level *level);
bool rpmi_perf_domain_can_set_level(struct rpmi_perf_domain *pd);
bool rpmi_perf_domain_has_fast_channel(struct rpmi_perf_domain *pd);
u32 rpmi_perf_domain_trans_latency_us(struct rpmi_perf_domain *pd);

int rpmi_perf_domain_get_level(struct rpmi_perf_domain *pd, u32 *level);
int rpmi_perf_domain_set_level(struct rpmi_perf_domain *pd, u32 level);
int rpmi_perf_domain_set_level_fast(struct rpmi_perf_domain *pd, u32 level);
int rpmi_perf_domain_get_limit(struct rpmi_perf_domain *pd, u32 *min, u32 *max);
int rpmi_perf_domain_set_limit(struct rpmi_perf_domain *pd, u32 min, u32 max);

int rpmi_perf_domain_level_to_freq(struct rpmi_perf_domain *pd, u32 level, u32 *khz);
int rpmi_perf_domain_freq_to_level(struct rpmi_perf_domain *pd, u32 khz, u32 *level);
int rpmi_perf_domain_opps_add(struct rpmi_perf_domain *pd, struct device *dev);

#else

static inline struct rpmi_perf_domain *
rpmi_perf_domain_by_id(struct rpmi_perf *perf, u32 id)
{
	return NULL;
}

static inline u32 rpmi_perf_num_domains(struct rpmi_perf *perf)
{
	return 0;
}

static inline const char *rpmi_perf_domain_name(struct rpmi_perf_domain *pd)
{
	return NULL;
}

static inline u32 rpmi_perf_domain_level_count(struct rpmi_perf_domain *pd)
{
	return 0;
}

static inline int rpmi_perf_domain_level_info(struct rpmi_perf_domain *pd, u32 idx,
					      struct rpmi_perf_level *level)
{
	return -EOPNOTSUPP;
}

static inline bool rpmi_perf_domain_can_set_level(struct rpmi_perf_domain *pd)
{
	return false;
}

static inline bool rpmi_perf_domain_has_fast_channel(struct rpmi_perf_domain *pd)
{
	return false;
}

static inline u32 rpmi_perf_domain_trans_latency_us(struct rpmi_perf_domain *pd)
{
	return 0;
}

static inline int rpmi_perf_domain_get_level(struct rpmi_perf_domain *pd, u32 *level)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_set_level(struct rpmi_perf_domain *pd, u32 level)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_set_level_fast(struct rpmi_perf_domain *pd, u32 level)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_get_limit(struct rpmi_perf_domain *pd, u32 *min, u32 *max)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_set_limit(struct rpmi_perf_domain *pd, u32 min, u32 max)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_level_to_freq(struct rpmi_perf_domain *pd, u32 level,
						 u32 *khz)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_freq_to_level(struct rpmi_perf_domain *pd, u32 khz,
						 u32 *level)
{
	return -EOPNOTSUPP;
}

static inline int rpmi_perf_domain_opps_add(struct rpmi_perf_domain *pd, struct device *dev)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_RISCV_RPMI_PERFORMANCE */

#endif /* _LINUX_RISCV_RPMI_PERFORMANCE_H_ */
