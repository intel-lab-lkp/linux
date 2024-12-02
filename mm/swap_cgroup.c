// SPDX-License-Identifier: GPL-2.0
#include <linux/swap_cgroup.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#include <linux/swapops.h> /* depends on mm.h include */

#define ID_PER_UNIT (sizeof(atomic_t) / sizeof(unsigned short))
struct swap_cgroup_unit {
	union {
		int raw;
		atomic_t val;
		unsigned short __id[ID_PER_UNIT];
	};
};

static DEFINE_MUTEX(swap_cgroup_mutex);

struct swap_cgroup {
	unsigned short		id;
};

struct swap_cgroup_ctrl {
	union {
		struct swap_cgroup_unit *units;
		unsigned short *map;
	};
};

static struct swap_cgroup_ctrl swap_cgroup_ctrl[MAX_SWAPFILES];

/*
 * SwapCgroup implements "lookup" and "exchange" operations.
 * In typical usage, this swap_cgroup is accessed via memcg's charge/uncharge
 * against SwapCache. At swap_free(), this is accessed directly from swap.
 *
 * This means,
 *  - we have no race in "exchange" when we're accessed via SwapCache because
 *    SwapCache(and its swp_entry) is under lock.
 *  - When called via swap_free(), there is no user of this entry and no race.
 * Then, we don't need lock around "exchange".
 *
 * TODO: we can push these buffers out to HIGHMEM.
 */
static unsigned short __swap_cgroup_xchg(void *map,
					 pgoff_t offset,
					 unsigned int new_id)
{
	unsigned int old_id;
	struct swap_cgroup_unit *units = map;
	struct swap_cgroup_unit *unit = &units[offset / ID_PER_UNIT];
	struct swap_cgroup_unit new, old = { .raw = atomic_read(&unit->val) };

	do {
		new.raw = old.raw;
		old_id = old.__id[offset % ID_PER_UNIT];
		new.__id[offset % ID_PER_UNIT] = new_id;
	} while (!atomic_try_cmpxchg(&unit->val, &old.raw, new.raw));

	return old_id;
}

/**
 * swap_cgroup_record - record mem_cgroup for a set of swap entries
 * @ent: the first swap entry to be recorded into
 * @id: mem_cgroup to be recorded
 * @nr_ents: number of swap entries to be recorded
 *
 * Returns old value at success, 0 at failure.
 * (Of course, old value can be 0.)
 */
unsigned short swap_cgroup_record(swp_entry_t ent, unsigned short id,
				  unsigned int nr_ents)
{
	struct swap_cgroup_ctrl *ctrl;
	pgoff_t offset = swp_offset(ent);
	pgoff_t end = offset + nr_ents;
	unsigned short old, iter;
	unsigned short *map;

	ctrl = &swap_cgroup_ctrl[swp_type(ent)];
	map = ctrl->map;

	old = READ_ONCE(map[offset]);
	do {
		iter = __swap_cgroup_xchg(map, offset, id);
		VM_BUG_ON(iter != old);
	} while (++offset != end);

	return old;
}

/**
 * lookup_swap_cgroup_id - lookup mem_cgroup id tied to swap entry
 * @ent: swap entry to be looked up.
 *
 * Returns ID of mem_cgroup at success. 0 at failure. (0 is invalid ID)
 */
unsigned short lookup_swap_cgroup_id(swp_entry_t ent)
{
	struct swap_cgroup_ctrl *ctrl;

	if (mem_cgroup_disabled())
		return 0;

	ctrl = &swap_cgroup_ctrl[swp_type(ent)];
	pgoff_t offset = swp_offset(ent);

	return READ_ONCE(ctrl->map[offset]);
}

int swap_cgroup_swapon(int type, unsigned long max_pages)
{
	struct swap_cgroup_unit *units;
	struct swap_cgroup_ctrl *ctrl;

	if (mem_cgroup_disabled())
		return 0;

	units = vzalloc(DIV_ROUND_UP(max_pages, ID_PER_UNIT) *
			sizeof(struct swap_cgroup_unit));
	if (!units)
		goto nomem;

	ctrl = &swap_cgroup_ctrl[type];
	mutex_lock(&swap_cgroup_mutex);
	ctrl->units = units;
	mutex_unlock(&swap_cgroup_mutex);

	return 0;
nomem:
	pr_info("couldn't allocate enough memory for swap_cgroup\n");
	pr_info("swap_cgroup can be disabled by swapaccount=0 boot option\n");
	return -ENOMEM;
}

void swap_cgroup_swapoff(int type)
{
	void *map;
	struct swap_cgroup_ctrl *ctrl;

	if (mem_cgroup_disabled())
		return;

	mutex_lock(&swap_cgroup_mutex);
	ctrl = &swap_cgroup_ctrl[type];
	map = ctrl->map;
	ctrl->map = NULL;
	mutex_unlock(&swap_cgroup_mutex);

	vfree(map);
}
