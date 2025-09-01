/* SPDX-License-Identifier: GPL-2.0 */
/*
 *
 * Generic netlink for energy model.
 *
 * Copyright (c) 2025 Valve Corporation.
 * Author: Changwoo Min <changwoo@igalia.com>
 */
#ifndef _EM_NETLINK_H
#define _EM_NETLINK_H

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_NET)
int em_notify_pd_created(const struct em_perf_domain *pd);
int em_notify_pd_deleted(const struct em_perf_domain *pd);
int em_notify_pd_updated(const struct em_perf_domain *pd);

#else
static inline int em_notify_pd_created(const struct em_perf_domain *pd)
{
	return 0;
}

static inline int em_notify_pd_deleted(const struct em_perf_domain *pd)
{
	return 0;
}

static inline int em_notify_pd_updated(const struct em_perf_domain *pd)
{
	return 0;
}
#endif

#endif /* _EM_NETLINK_H */
