// SPDX-License-Identifier: GPL-2.0
/*
 * Memory hotplug support via sclp
 *
 * Copyright IBM Corp. 2025
 */

#define KMSG_COMPONENT "sclp_mem"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/cpufeature.h>
#include <linux/container_of.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <asm/facility.h>
#include <asm/page.h>
#include <asm/page-states.h>
#include <asm/sclp.h>

#include "sclp.h"

#define SCLP_CMDW_ASSIGN_STORAGE		0x000d0001
#define SCLP_CMDW_UNASSIGN_STORAGE		0x000c0001

static LIST_HEAD(sclp_mem_list);
static u8 sclp_max_storage_id;
static DECLARE_BITMAP(sclp_storage_ids, 256);

struct memory_increment {
	struct list_head list;
	u16 rn;
	int standby;
};

struct mblock {
	struct kobject kobj;
	unsigned int id;
	unsigned int memmap_on_memory;
	unsigned int config;
};

struct memory_block_arg {
	struct mblock *mblocks;
	struct kset *kset;
};

struct assign_storage_sccb {
	struct sccb_header header;
	u16 rn;
} __packed;

struct attach_storage_sccb {
	struct sccb_header header;
	u16 :16;
	u16 assigned;
	u32 :32;
	u32 entries[];
} __packed;

int arch_get_memory_phys_device(unsigned long start_pfn)
{
	if (!sclp.rzm)
		return 0;
	return PFN_PHYS(start_pfn) >> ilog2(sclp.rzm);
}

static unsigned long rn2addr(u16 rn)
{
	return (unsigned long)(rn - 1) * sclp.rzm;
}

static int do_assign_storage(sclp_cmdw_t cmd, u16 rn)
{
	struct assign_storage_sccb *sccb;
	int rc;

	sccb = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!sccb)
		return -ENOMEM;
	sccb->header.length = PAGE_SIZE;
	sccb->rn = rn;
	rc = sclp_sync_request_timeout(cmd, sccb, SCLP_QUEUE_INTERVAL);
	if (rc)
		goto out;
	switch (sccb->header.response_code) {
	case 0x0020:
	case 0x0120:
		break;
	default:
		pr_warn("assign storage failed (cmd=0x%08x, response=0x%04x, rn=0x%04x)\n",
			cmd, sccb->header.response_code, rn);
		rc = -EIO;
		break;
	}
out:
	free_page((unsigned long)sccb);
	return rc;
}

static int sclp_assign_storage(u16 rn)
{
	unsigned long start;
	int rc;

	rc = do_assign_storage(SCLP_CMDW_ASSIGN_STORAGE, rn);
	if (rc)
		return rc;
	start = rn2addr(rn);
	storage_key_init_range(start, start + sclp.rzm);
	return 0;
}

static int sclp_unassign_storage(u16 rn)
{
	return do_assign_storage(SCLP_CMDW_UNASSIGN_STORAGE, rn);
}

static int sclp_attach_storage(u8 id)
{
	struct attach_storage_sccb *sccb;
	int rc, i;

	sccb = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!sccb)
		return -ENOMEM;
	sccb->header.length = PAGE_SIZE;
	sccb->header.function_code = 0x40;
	rc = sclp_sync_request_timeout(0x00080001 | id << 8, sccb,
				       SCLP_QUEUE_INTERVAL);
	if (rc)
		goto out;
	switch (sccb->header.response_code) {
	case 0x0020:
		set_bit(id, sclp_storage_ids);
		for (i = 0; i < sccb->assigned; i++) {
			if (sccb->entries[i])
				sclp_unassign_storage(sccb->entries[i] >> 16);
		}
		break;
	default:
		rc = -EIO;
		break;
	}
out:
	free_page((unsigned long)sccb);
	return rc;
}

static int sclp_mem_change_state(unsigned long start, unsigned long size,
				 int online)
{
	struct memory_increment *incr;
	unsigned long istart;
	int rc = 0;

	list_for_each_entry(incr, &sclp_mem_list, list) {
		istart = rn2addr(incr->rn);
		if (start + size - 1 < istart)
			break;
		if (start > istart + sclp.rzm - 1)
			continue;
		if (online)
			rc |= sclp_assign_storage(incr->rn);
		else
			sclp_unassign_storage(incr->rn);
		if (rc == 0)
			incr->standby = online ? 0 : 1;
	}
	return rc ? -EIO : 0;
}

static bool contains_standby_increment(unsigned long start, unsigned long end)
{
	struct memory_increment *incr;
	unsigned long istart;

	list_for_each_entry(incr, &sclp_mem_list, list) {
		istart = rn2addr(incr->rn);
		if (end - 1 < istart)
			continue;
		if (start > istart + sclp.rzm - 1)
			continue;
		if (incr->standby)
			return true;
	}
	return false;
}

static int sclp_mem_notifier(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	unsigned long start, size;
	struct memory_notify *arg;
	int rc = 0;

	arg = data;
	start = arg->start_pfn << PAGE_SHIFT;
	size = arg->nr_pages << PAGE_SHIFT;
	switch (action) {
	case MEM_GOING_OFFLINE:
		/*
		 * Do not allow to set memory blocks offline that contain
		 * standby memory. This is done to simplify the "memory online"
		 * case.
		 */
		if (contains_standby_increment(start, start + size))
			rc = -EPERM;
		break;
	default:
		break;
	}
	return rc ? NOTIFY_BAD : NOTIFY_OK;
}

static ssize_t config_mblock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct mblock *mblock = container_of(kobj, struct mblock, kobj);

	return sysfs_emit(buf, "%u\n", READ_ONCE(mblock->config));
}

static ssize_t config_mblock_store(struct kobject *kobj, struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long long addr, block_size;
	struct memory_block *mem;
	struct mblock *mblock;
	unsigned char id;
	bool value;
	int rc;

	rc = kstrtobool(buf, &value);
	if (rc)
		return rc;
	mblock = container_of(kobj, struct mblock, kobj);
	block_size = memory_block_size_bytes();
	addr = mblock->id * block_size;
	/*
	 * Hold device_hotplug_lock when adding/removing memory blocks.
	 * Additionally, also protect calls to find_memory_block() and
	 * sclp_attach_storage().
	 */
	rc = lock_device_hotplug_sysfs();
	if (rc)
		goto out;
	for_each_clear_bit(id, sclp_storage_ids, sclp_max_storage_id + 1)
		sclp_attach_storage(id);
	if (value) {
		if (mblock->config)
			goto out_unlock;
		rc = sclp_mem_change_state(addr, block_size, 1);
		if (rc)
			goto out_unlock;
		/*
		 * Set entire memory block CMMA state to nodat. Later, when
		 * page tables pages are allocated via __add_memory(), those
		 * regions are marked __arch_set_page_dat().
		 */
		__arch_set_page_nodat((void *)__va(addr), block_size >> PAGE_SHIFT);
		rc = __add_memory(0, addr, block_size,
				  mblock->memmap_on_memory ?
				  MHP_MEMMAP_ON_MEMORY | MHP_OFFLINE_INACCESSIBLE : MHP_NONE);
		if (rc)
			goto out_unlock;
		mem = find_memory_block(pfn_to_section_nr(PFN_DOWN(addr)));
		put_device(&mem->dev);
		WRITE_ONCE(mblock->config, 1);
	} else {
		if (!mblock->config)
			goto out_unlock;
		mem = find_memory_block(pfn_to_section_nr(PFN_DOWN(addr)));
		if (mem->state != MEM_OFFLINE) {
			put_device(&mem->dev);
			rc = -EBUSY;
			goto out_unlock;
		}
		/* drop the ref just got via find_memory_block() */
		put_device(&mem->dev);
		sclp_mem_change_state(addr, block_size, 0);
		__remove_memory(addr, block_size);
		WRITE_ONCE(mblock->config, 0);
	}
out_unlock:
	unlock_device_hotplug();
out:
	return rc ? rc : count;
}

static ssize_t memmap_on_memory_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct mblock *mblock = container_of(kobj, struct mblock, kobj);

	return sysfs_emit(buf, "%u\n", READ_ONCE(mblock->memmap_on_memory));
}

static ssize_t memmap_on_memory_store(struct kobject *kobj, struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned long block_size;
	struct memory_block *mem;
	struct mblock *mblock;
	bool value;
	int rc;

	rc = kstrtobool(buf, &value);
	if (rc)
		return rc;
	rc = lock_device_hotplug_sysfs();
	if (rc)
		return rc;
	block_size = memory_block_size_bytes();
	mblock = container_of(kobj, struct mblock, kobj);
	mem = find_memory_block(pfn_to_section_nr(PFN_DOWN(mblock->id * block_size)));
	if (!mem) {
		WRITE_ONCE(mblock->memmap_on_memory, value);
	} else {
		put_device(&mem->dev);
		rc = -EBUSY;
	}
	unlock_device_hotplug();
	return rc ? rc : count;
}

static void mblock_sysfs_release(struct kobject *kobj)
{
	struct mblock *mblock = container_of(kobj, struct mblock, kobj);

	kfree(mblock);
}

static const struct kobj_type ktype = {
	.release = mblock_sysfs_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct kobj_attribute memmap_attr =
	__ATTR(memmap_on_memory, 0644, memmap_on_memory_show, memmap_on_memory_store);
static struct kobj_attribute config_attr =
	__ATTR(config, 0644, config_mblock_show, config_mblock_store);

static struct attribute *mblock_attrs[] = {
	&memmap_attr.attr,
	&config_attr.attr,
	NULL,
};

static struct attribute_group mblock_attr_group = {
	.attrs = mblock_attrs,
};

static int create_mblock(struct mblock *mblock, struct kset *kset,
			 unsigned int id, bool config, bool memmap_on_memory)
{
	int rc;

	mblock->memmap_on_memory = memmap_on_memory;
	mblock->config = config;
	mblock->id = id;
	kobject_init(&mblock->kobj, &ktype);
	rc = kobject_add(&mblock->kobj, &kset->kobj, "memory%d", id);
	if (rc)
		return rc;
	rc = sysfs_create_group(&mblock->kobj, &mblock_attr_group);
	if (rc)
		kobject_put(&mblock->kobj);
	return rc;
}

/*
 * Create /sys/firmware/memory/memoryX for boottime configured online memory
 * blocks
 */
static int create_online_mblock(struct memory_block *mem, void *argument)
{
	struct memory_block_arg *arg;
	struct mblock *mblocks;
	struct kset *kset;
	unsigned int id;

	id = mem->dev.id;
	arg = (struct memory_block_arg *)argument;
	mblocks = arg->mblocks;
	kset = arg->kset;
	return create_mblock(&mblocks[id], kset, id, true, false);
}

static int __init create_initial_online_mblocks(struct mblock *mblocks, struct kset *kset)
{
	struct memory_block_arg arg;

	arg.mblocks = mblocks;
	arg.kset = kset;
	return for_each_memory_block(&arg, create_online_mblock);
}

static struct mblock * __init allocate_mblocks(void)
{
	u64 max_mblocks;
	u64 block_size;

	block_size = memory_block_size_bytes();
	max_mblocks = roundup(sclp.rnmax * sclp.rzm, block_size) / block_size;
	return kcalloc(max_mblocks, sizeof(struct mblock), GFP_KERNEL);
}

static struct notifier_block sclp_mem_nb = {
	.notifier_call = sclp_mem_notifier,
};

static void __init align_to_block_size(unsigned long *start,
				       unsigned long *size,
				       unsigned long alignment)
{
	unsigned long start_align, size_align;

	start_align = roundup(*start, alignment);
	size_align = rounddown(*start + *size, alignment) - start_align;

	pr_info("Standby memory at 0x%lx (%luM of %luM usable)\n",
		*start, size_align >> 20, *size >> 20);
	*start = start_align;
	*size = size_align;
}

static int __init create_standby_mblocks_merged(struct mblock *mblocks,
						struct kset *kset, u16 rn)
{
	unsigned long start, size, addr, block_size;
	static u16 first_rn, num;
	unsigned int id;
	int rc = 0;

	if (rn && first_rn && (first_rn + num == rn)) {
		num++;
		return rc;
	}
	if (!first_rn)
		goto skip_add;
	start = rn2addr(first_rn);
	size = (unsigned long)num * sclp.rzm;
	if (start >= ident_map_size)
		goto skip_add;
	if (start + size > ident_map_size)
		size = ident_map_size - start;
	block_size = memory_block_size_bytes();
	align_to_block_size(&start, &size, block_size);
	if (!size)
		goto skip_add;
	for (addr = start; addr < start + size; addr += block_size) {
		id = addr / block_size;
		rc = create_mblock(&mblocks[id], kset, id, false, mhp_supports_memmap_on_memory());
		if (rc)
			break;
	}
skip_add:
	first_rn = rn;
	num = 1;
	return rc;
}

static int __init create_standby_mblocks(struct mblock *mblocks, struct kset *kset)
{
	struct memory_increment *incr;
	int rc = 0;

	list_for_each_entry(incr, &sclp_mem_list, list) {
		if (incr->standby)
			rc = create_standby_mblocks_merged(mblocks, kset, incr->rn);
		if (rc)
			goto out;
	}
	rc = create_standby_mblocks_merged(mblocks, kset, 0);
out:
	return rc;
}

static void __init insert_increment(u16 rn, int standby, int assigned)
{
	struct memory_increment *incr, *new_incr;
	struct list_head *prev;
	u16 last_rn;

	new_incr = kzalloc(sizeof(*new_incr), GFP_KERNEL);
	if (!new_incr)
		return;
	new_incr->rn = rn;
	new_incr->standby = standby;
	last_rn = 0;
	prev = &sclp_mem_list;
	list_for_each_entry(incr, &sclp_mem_list, list) {
		if (assigned && incr->rn > rn)
			break;
		if (!assigned && incr->rn - last_rn > 1)
			break;
		last_rn = incr->rn;
		prev = &incr->list;
	}
	if (!assigned)
		new_incr->rn = last_rn + 1;
	if (new_incr->rn > sclp.rnmax) {
		kfree(new_incr);
		return;
	}
	list_add(&new_incr->list, prev);
}

static int __init sclp_setup_memory(void)
{
	struct read_storage_sccb *sccb;
	int i, id, assigned, rc;
	struct mblock *mblocks;
	struct kset *kset;

	/* No standby memory in kdump mode */
	if (oldmem_data.start)
		return 0;
	if ((sclp.facilities & 0xe00000000000UL) != 0xe00000000000UL)
		return 0;
	rc = -ENOMEM;
	sccb = (void *)__get_free_page(GFP_KERNEL | GFP_DMA);
	if (!sccb)
		goto out;
	assigned = 0;
	for (id = 0; id <= sclp_max_storage_id; id++) {
		memset(sccb, 0, PAGE_SIZE);
		sccb->header.length = PAGE_SIZE;
		rc = sclp_sync_request(SCLP_CMDW_READ_STORAGE_INFO | id << 8, sccb);
		if (rc)
			goto out;
		switch (sccb->header.response_code) {
		case 0x0010:
			set_bit(id, sclp_storage_ids);
			for (i = 0; i < sccb->assigned; i++) {
				if (!sccb->entries[i])
					continue;
				assigned++;
				insert_increment(sccb->entries[i] >> 16, 0, 1);
			}
			break;
		case 0x0310:
			break;
		case 0x0410:
			for (i = 0; i < sccb->assigned; i++) {
				if (!sccb->entries[i])
					continue;
				assigned++;
				insert_increment(sccb->entries[i] >> 16, 1, 1);
			}
			break;
		default:
			rc = -EIO;
			break;
		}
		if (!rc)
			sclp_max_storage_id = sccb->max_id;
	}
	if (rc || list_empty(&sclp_mem_list))
		goto out;
	for (i = 1; i <= sclp.rnmax - assigned; i++)
		insert_increment(0, 1, 0);
	rc = register_memory_notifier(&sclp_mem_nb);
	if (rc)
		goto out;
	mblocks = allocate_mblocks();
	if (!mblocks) {
		rc = -ENOMEM;
		goto out;
	}
	kset = kset_create_and_add("memory", NULL, firmware_kobj);
	if (!kset) {
		rc = -ENOMEM;
		goto out;
	}
	rc = create_initial_online_mblocks(mblocks, kset);
	if (rc)
		goto out;
	rc = create_standby_mblocks(mblocks, kset);
out:
	free_page((unsigned long)sccb);
	return rc;
}
__initcall(sclp_setup_memory);
