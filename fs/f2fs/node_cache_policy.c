// SPDX-License-Identifier: GPL-2.0
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/shrinker.h>
#include <linux/string.h>

#include "node_cache_policy.h"

#define F2FS_NC_RECLAIM_SCALE_PCT	25U
#define F2FS_NC_SCORE_HEADROOM		4U

void f2fs_nc_config_defaults(struct f2fs_nc_config *cfg)
{
	*cfg = (struct f2fs_nc_config) {
		.compression_threshold_pct = F2FS_NC_DEFAULT_THRESHOLD_PCT,
		.compression_interval_ms = F2FS_NC_DEFAULT_INTERVAL_MS,
	};
}

int f2fs_nc_config_set(struct f2fs_nc_config *out,
		       const struct f2fs_nc_config *old, enum f2fs_nc_param id, u64 value)
{
	struct f2fs_nc_config new;

	if (!out || !old)
		return -EINVAL;
	new = *old;
	switch (id) {
	case F2FS_NC_PARAM_THRESHOLD:
		if (value > F2FS_NC_PERCENT_MAX)
			return -ERANGE;
		new.compression_threshold_pct = value;
		break;
	case F2FS_NC_PARAM_INTERVAL:
		if (value < F2FS_NC_MIN_INTERVAL_MS ||
		    value > F2FS_NC_MAX_INTERVAL_MS)
			return -ERANGE;
		new.compression_interval_ms = value;
		break;
	default:
		return -EINVAL;
	}
	*out = new;
	return 0;
}

u64 f2fs_nc_config_get(const struct f2fs_nc_config *cfg,
		       enum f2fs_nc_param id)
{
	switch (id) {
	case F2FS_NC_PARAM_THRESHOLD:
		return cfg->compression_threshold_pct;
	case F2FS_NC_PARAM_INTERVAL:
		return cfg->compression_interval_ms;
	default:
		return 0;
	}
}

static void f2fs_nc_queue_weights(u32 blocksize,
				  u64 weight[F2FS_NC_NR_QUEUES])
{
	weight[F2FS_NC_RAW] = blocksize;
	weight[F2FS_NC_256] = F2FS_NC_BUCKET_256_SIZE *
			F2FS_NC_RECLAIM_SCALE_PCT / F2FS_NC_PERCENT_MAX;
	weight[F2FS_NC_512] = F2FS_NC_BUCKET_512_SIZE *
			F2FS_NC_RECLAIM_SCALE_PCT / F2FS_NC_PERCENT_MAX;
	weight[F2FS_NC_1024] = F2FS_NC_BUCKET_1024_SIZE *
			F2FS_NC_RECLAIM_SCALE_PCT / F2FS_NC_PERCENT_MAX;
}

static u64 f2fs_nc_queue_mass(unsigned long nr, u64 weight,
			      unsigned long budget)
{
	u64 limit;
	u64 count;

	if (!nr || !weight || !budget)
		return 0;
	/* Keep both the quota product and signed score arithmetic bounded. */
	limit = min_t(u64, S64_MAX / F2FS_NC_SCORE_HEADROOM /
			F2FS_NC_NR_QUEUES,
			div64_u64(U64_MAX, budget));
	if (limit < weight)
		return 1;
	count = min_t(u64, nr, div64_u64(limit, weight));
	return count * weight;
}

u64 f2fs_nc_effective_count(const unsigned long nr[F2FS_NC_NR_QUEUES],
			    u32 blocksize)
{
	u64 weight[F2FS_NC_NR_QUEUES];
	u64 cap = (u64)SHRINK_EMPTY - 1;
	u64 count = 0;
	u64 remainder = 0;
	u64 carry;
	u64 carry_remainder;
	unsigned int i;

	if (!blocksize)
		return 0;
	f2fs_nc_queue_weights(blocksize, weight);
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
		u64 quotient, count_remainder;

		quotient = mul_u64_u64_div_u64(nr[i], weight[i], blocksize);
		if (quotient > cap - count)
			return cap;
		count += quotient;
		div64_u64_rem(nr[i], blocksize, &count_remainder);
		remainder += count_remainder * weight[i] % blocksize;
	}
	carry = div64_u64_rem(remainder, blocksize, &carry_remainder);
	if (carry_remainder)
		carry++;
	if (carry > cap - count)
		return cap;
	return count + carry;
}

void f2fs_nc_scan_quotas(const unsigned long nr[F2FS_NC_NR_QUEUES],
			 u32 blocksize, unsigned long requested,
		s64 credit[F2FS_NC_NR_QUEUES],
		unsigned long quota[F2FS_NC_NR_QUEUES])
{
	u64 weight[F2FS_NC_NR_QUEUES];
	u64 mass[F2FS_NC_NR_QUEUES], total_mass;
	s64 score[F2FS_NC_NR_QUEUES];
	unsigned long population = 0, budget, assigned = 0;
	unsigned int round, i;

	memset(quota, 0, sizeof(*quota) * F2FS_NC_NR_QUEUES);
	f2fs_nc_queue_weights(blocksize, weight);
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
		if (!nr[i] || !weight[i])
			credit[i] = 0;
		if (weight[i] &&
		    check_add_overflow(population, nr[i], &population))
			population = ULONG_MAX;
	}
	budget = min(requested, population);
	if (!budget || !blocksize)
		return;
	total_mass = 0;
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
		mass[i] = f2fs_nc_queue_mass(nr[i], weight[i], budget);
		total_mass += mass[i];
	}
	if (!total_mass)
		return;
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++)
		score[i] = mass[i] ? clamp_t(s64, credit[i],
						-(s64)total_mass,
						(s64)total_mass) : 0;

	for (round = 0; round < F2FS_NC_NR_QUEUES + 1 && assigned < budget;
	     round++) {
		u64 active_mass = 0;
		unsigned long before = assigned;
		bool capped = false;

		for (i = 0; i < F2FS_NC_NR_QUEUES; i++)
			if (mass[i] && quota[i] < nr[i])
				active_mass += mass[i];
		if (!active_mass)
			break;
		for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
			unsigned long capacity, share;
			u64 product, quotient, remainder;

			if (!mass[i] || quota[i] >= nr[i])
				continue;
			capacity = nr[i] - quota[i];
			quotient = mul_u64_u64_div_u64(budget - assigned,
						       mass[i], active_mass);
			share = min_t(u64, quotient, capacity);
			if (share < quotient)
				capped = true;
			quota[i] += share;
			before += share;
			product = (budget - assigned) * mass[i];
			remainder = product - quotient * active_mass;
			score[i] = clamp_t(s64, score[i], -(s64)active_mass,
					   (s64)active_mass) + (s64)remainder;
		}
		assigned = before;
		while (assigned < budget) {
			unsigned int best = F2FS_NC_NR_QUEUES;

			for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
				if (!mass[i] || quota[i] >= nr[i])
					continue;
				if (best == F2FS_NC_NR_QUEUES ||
				    score[i] > score[best])
					best = i;
			}
			if (best == F2FS_NC_NR_QUEUES)
				break;
			quota[best]++;
			score[best] -= active_mass;
			assigned++;
			if (capped)
				break;
		}
		if (!capped || assigned == budget)
			break;
	}
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++)
		credit[i] = mass[i] ? score[i] : 0;
}
