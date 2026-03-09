/* SPDX-License-Identifier: GPL-2.0-only */
#include "entry_sync.h"
#include <linux/args.h>
#include <linux/bitops.h>

#ifndef entry_sync_writer
#define entry_sync_writer entry_sync_writer64
#define quanta_t __le64
#define NS(name) CONCATENATE(name, 64)
#endif

/*
 * Figure out if we can do a hitless update of entry to become target. Returns a
 * bit mask where 1 indicates that a quanta word needs to be set disruptively.
 * unused_update is an intermediate value of entry that has unused bits set to
 * their new values.
 */
static u8 NS(entry_quanta_diff)(struct entry_sync_writer *writer,
				const quanta_t *entry, const quanta_t *target,
				quanta_t *unused_update, quanta_t *memory)
{
	quanta_t *target_used = memory + writer->num_quantas * 1;
	quanta_t *cur_used = memory + writer->num_quantas * 2;
	u8 used_qword_diff = 0;
	unsigned int i;

	writer->ops->get_used(entry, cur_used);
	writer->ops->get_used(target, target_used);

	for (i = 0; i != writer->num_quantas; i++) {
		/*
		 * Check that masks are up to date, the make functions are not
		 * allowed to set a bit to 1 if the used function doesn't say it
		 * is used.
		 */
		WARN_ON_ONCE(target[i] & ~target_used[i]);

		/* Bits can change because they are not currently being used */
		unused_update[i] = (entry[i] & cur_used[i]) |
				   (target[i] & ~cur_used[i]);
		/*
		 * Each bit indicates that a used bit in a qword needs to be
		 * changed after unused_update is applied.
		 */
		if ((unused_update[i] & target_used[i]) != target[i])
			used_qword_diff |= 1 << i;
	}
	return used_qword_diff;
}

/*
 * Update the entry to the target configuration. The transition from the current
 * entry to the target entry takes place over multiple steps that attempts to
 * make the transition hitless if possible. This function takes care not to
 * create a situation where the HW can perceive a corrupted entry. HW is only
 * required to have a quanta-bit atomicity with stores from the CPU, while
 * entries are many quanta bit values big.
 *
 * The difference between the current value and the target value is analyzed to
 * determine which of three updates are required - disruptive, hitless or no
 * change.
 *
 * In the most general disruptive case we can make any update in three steps:
 *  - Disrupting the entry (V=0)
 *  - Fill now unused quanta words, except qword 0 which contains V
 *  - Make qword 0 have the final value and valid (V=1) with a single 64
 *    bit store
 *
 * However this disrupts the HW while it is happening. There are several
 * interesting cases where a STE/CD can be updated without disturbing the HW
 * because only a small number of bits are changing (S1DSS, CONFIG, etc) or
 * because the used bits don't intersect. We can detect this by calculating how
 * many 64 bit values need update after adjusting the unused bits and skip the
 * V=0 process. This relies on the IGNORED behavior described in the
 * specification.
 */
void NS(entry_sync_write)(struct entry_sync_writer *writer, quanta_t *entry,
			  const quanta_t *target, quanta_t *memory,
			  size_t memory_len)
{
	quanta_t *unused_update = memory + writer->num_quantas * 0;
	u8 used_qword_diff;

	if (WARN_ON(memory_len !=
		    ENTRY_SYNC_MEMORY_LEN(writer) * sizeof(*memory)))
		return;

	used_qword_diff = NS(entry_quanta_diff)(writer, entry, target,
						unused_update, memory);
	if (hweight8(used_qword_diff) == 1) {
		/*
		 * Only one quanta needs its used bits to be changed. This is a
		 * hitless update, update all bits the current entry is ignoring
		 * to their new values, then update a single "critical quanta"
		 * to change the entry and finally 0 out any bits that are now
		 * unused in the target configuration.
		 */
		unsigned int critical_qword_index = ffs(used_qword_diff) - 1;

		/*
		 * Skip writing unused bits in the critical quanta since we'll
		 * be writing it in the next step anyways. This can save a sync
		 * when the only change is in that quanta.
		 */
		unused_update[critical_qword_index] =
			entry[critical_qword_index];
		NS(entry_set)(writer, entry, unused_update, 0,
			      writer->num_quantas);
		NS(entry_set)(writer, entry, target, critical_qword_index, 1);
		NS(entry_set)(writer, entry, target, 0, writer->num_quantas);
	} else if (used_qword_diff) {
		/*
		 * At least two quantas need their inuse bits to be changed.
		 * This requires a breaking update, zero the V bit, write all
		 * qwords but 0, then set qword 0
		 */
		unused_update[writer->vbit_quanta] = 0;
		NS(entry_set)(writer, entry, unused_update, writer->vbit_quanta, 1);

		if (writer->vbit_quanta != 0)
			NS(entry_set)(writer, entry, target, 0,
				      writer->vbit_quanta - 1);
		if (writer->vbit_quanta != writer->num_quantas)
			NS(entry_set)(writer, entry, target,
				      writer->vbit_quanta,
				      writer->num_quantas - 1);

		NS(entry_set)(writer, entry, target, writer->vbit_quanta, 1);
	} else {
		/*
		 * No inuse bit changed. Sanity check that all unused bits are 0
		 * in the entry. The target was already sanity checked by
		 * entry_quanta_diff().
		 */
		WARN_ON_ONCE(NS(entry_set)(writer, entry, target, 0,
					   writer->num_quantas));
	}
}
EXPORT_SYMBOL(NS(entry_sync_write));

#undef entry_sync_writer
#undef quanta_t
#undef NS
