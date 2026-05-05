// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include "rmr-clt.h"

int rmr_clt_reset_read_retries(struct rmr_clt_stats *stats, bool enable)
{
	if (unlikely(!enable))
		return -EINVAL;

	atomic_set(&stats->read_retries, 0);

	return 0;
}

ssize_t rmr_clt_stats_read_retries_to_str(
	struct rmr_clt_stats *stats, char *page)
{
	return sysfs_emit(page, "%u\n",
			 atomic_read(&stats->read_retries));
}

