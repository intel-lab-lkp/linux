// SPDX-License-Identifier: GPL-2.0
/*
 * pghot: Precision mode
 *
 * 4 byte hotness record per PFN (u32)
 * NID, time and frequency tracked as part of the record.
 */

#include <linux/pghot.h>
#include <linux/jiffies.h>
#include <linux/memory-tiers.h>

bool pghot_nid_valid(int nid)
{
	if (nid != NUMA_NO_NODE &&
	    (!numa_valid_node(nid) || nid > PGHOT_NID_MAX ||
	     !node_online(nid) || !node_is_toptier(nid)))
		return false;

	return true;
}

unsigned long pghot_access_latency(unsigned long old_time, unsigned long time)
{
	return jiffies_to_msecs((time - old_time) & PGHOT_TIME_MASK);
}

bool pghot_update_record(phi_t *phi, int nid, unsigned long now)
{
	phi_t freq, old_freq, hotness, old_hotness, old_time;
	phi_t time = now & PGHOT_TIME_MASK;

	nid = (nid == NUMA_NO_NODE) ? pghot_target_nid : nid;
	old_hotness = READ_ONCE(*phi);

	do {
		bool new_window = false;

		hotness = old_hotness;
		old_freq = (hotness >> PGHOT_FREQ_SHIFT) & PGHOT_FREQ_MASK;
		old_time = (hotness >> PGHOT_TIME_SHIFT) & PGHOT_TIME_MASK;

		if (pghot_access_latency(old_time, time) > sysctl_pghot_freq_window)
			new_window = true;

		if (new_window)
			freq = 1;
		else if (old_freq < PGHOT_FREQ_MAX)
			freq = old_freq + 1;
		else
			freq = old_freq;

		hotness &= ~(PGHOT_NID_MASK << PGHOT_NID_SHIFT);
		hotness &= ~(PGHOT_FREQ_MASK << PGHOT_FREQ_SHIFT);
		hotness &= ~(PGHOT_TIME_MASK << PGHOT_TIME_SHIFT);

		hotness |= (nid & PGHOT_NID_MASK) << PGHOT_NID_SHIFT;
		hotness |= (freq & PGHOT_FREQ_MASK) << PGHOT_FREQ_SHIFT;
		hotness |= (time & PGHOT_TIME_MASK) << PGHOT_TIME_SHIFT;

		if (freq >= pghot_freq_threshold)
			hotness |= BIT(PGHOT_MIGRATE_READY);
	} while (unlikely(!try_cmpxchg(phi, &old_hotness, hotness)));
	return !!(hotness & BIT(PGHOT_MIGRATE_READY));
}

int pghot_get_record(phi_t *phi, int *nid, int *freq, unsigned long *time)
{
	phi_t old_hotness, hotness = 0;

	old_hotness = READ_ONCE(*phi);
	do {
		if (!(old_hotness & BIT(PGHOT_MIGRATE_READY)))
			return -EINVAL;
	} while (unlikely(!try_cmpxchg(phi, &old_hotness, hotness)));

	*nid = (old_hotness >> PGHOT_NID_SHIFT) & PGHOT_NID_MASK;
	*freq = (old_hotness >> PGHOT_FREQ_SHIFT) & PGHOT_FREQ_MASK;
	*time = (old_hotness >> PGHOT_TIME_SHIFT) & PGHOT_TIME_MASK;
	return 0;
}
