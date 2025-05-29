// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Generic netlink for energy model.
 *
 * Copyright (c) 2025 Valve Corporation.
 * Author: Changwoo Min <changwoo@igalia.com>
 */

enum em_genl_multicast_groups {
	EM_GENL_EVENT_GROUP = 0,
	EM_GENL_MAX_GROUP = EM_GENL_EVENT_GROUP,
};

/* Netlink notification function */
#ifdef CONFIG_ENERGY_MODEL_NETLINK
int __init em_netlink_init(void);
void __init em_netlink_exit(void);

int em_notify_pd_create(const struct em_perf_domain *pd);
int em_notify_pd_delete(const struct em_perf_domain *pd);
int em_notify_pd_update(const struct em_perf_domain *pd);
#else
static inline int em_netlink_init(void)
{
	return 0;
}

static inline void em_netlink_exit(void) {}

static inline int em_notify_pd_create(const struct em_perf_domain *pd)
{
	return 0;
}

static inline int em_notify_pd_delete(const struct em_perf_domain *pd)
{
	return 0;
}

static inline int em_notify_pd_update(const struct em_perf_domain *pd)
{
	return 0;
}
#endif /* CONFIG_ENERGY_MODEL_NETLINK */
