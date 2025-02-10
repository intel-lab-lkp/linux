// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Intel Corporation.
 *
 * Memory Range and Region Mapping (MRRM) structure
 *
 * Parse and report the platform's MRRM table in /sys.
 */

#define pr_fmt(fmt) "acpi/mrrm: " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/sysfs.h>

static int max_mem_region = -ENOENT;

/* Access for use by resctrl file system */
int mrrm_max_mem_region(void)
{
	return max_mem_region;
}

struct mrrm_mem_range_entry {
	struct device dev;
	u64 base;
	u64 length;
	u8  local_region_id;
	u8  remote_region_id;
};

static struct mrrm_mem_range_entry *mrrm_mem_range_entry;
static u32 mrrm_mem_entry_num;

static __init int acpi_parse_mrrm(struct acpi_table_header *table)
{
	struct acpi_table_mrrm_mem_range_entry *mre_entry;
	struct acpi_table_mrrm *mrrm;
	void *mre, *mrrm_end;
	int mre_count = 0;

	mrrm = (struct acpi_table_mrrm *)table;
	if (!mrrm)
		return -ENODEV;

	if (mrrm->flags & ACPI_MRRM_FLAGS_REGION_ASSIGNMENT_OS)
		return -EOPNOTSUPP;

	mrrm_end = (void *)mrrm + mrrm->header.length - 1;
	mre = (void *)mrrm + sizeof(struct acpi_table_mrrm);
	while (mre < mrrm_end) {
		mre_entry = mre;
		mre_count++;
		mre += mre_entry->length;
	}
	if (!mre_count) {
		pr_info(FW_BUG "No ranges listed in MRRM table\n");
		return -EINVAL;
	}

	mrrm_mem_range_entry = kmalloc_array(mre_count, sizeof(*mrrm_mem_range_entry),
					     GFP_KERNEL | __GFP_ZERO);
	if (!mrrm_mem_range_entry)
		return -ENOMEM;

	mre = (void *)mrrm + sizeof(struct acpi_table_mrrm);
	while (mre < mrrm_end) {
		struct mrrm_mem_range_entry *e;

		mre_entry = mre;
		e = mrrm_mem_range_entry + mrrm_mem_entry_num;

		e->base = ((u64)mre_entry->base_addr_high << 32) + mre_entry->base_addr_low;
		e->length = ((u64)mre_entry->len_high << 32) + mre_entry->len_low;

		if (mre_entry->region_id_flags & ACPI_MRRM_VALID_REGION_ID_FLAGS_LOCAL)
			e->local_region_id = mre_entry->local_region_id;
		else
			e->local_region_id = -1;
		if (mre_entry->region_id_flags & ACPI_MRRM_VALID_REGION_ID_FLAGS_REMOTE)
			e->remote_region_id = mre_entry->remote_region_id;
		else
			e->remote_region_id = -1;

		mrrm_mem_entry_num++;
		mre += mre_entry->length;
	}

	max_mem_region = mrrm->max_mem_region;

	return 0;
}

#define RANGE_ATTR(name)						\
static ssize_t name##_show(struct device *dev,				\
			  struct device_attribute *attr, char *buf)	\
{									\
	struct mrrm_mem_range_entry *mre;				\
									\
	mre = container_of(dev, struct mrrm_mem_range_entry, dev);	\
	return sysfs_emit(buf, "0x%lx\n", (unsigned long)mre->name);	\
}									\
static DEVICE_ATTR_RO(name)

RANGE_ATTR(base);
RANGE_ATTR(length);
RANGE_ATTR(local_region_id);
RANGE_ATTR(remote_region_id);

static struct attribute *memory_range_attrs[] = {
	&dev_attr_base.attr,
	&dev_attr_length.attr,
	&dev_attr_local_region_id.attr,
	&dev_attr_remote_region_id.attr,
	NULL
};

ATTRIBUTE_GROUPS(memory_range);

static __init int add_boot_memory_ranges(void)
{
	char name[16];
	int i, ret;

	for (i = 0; i < mrrm_mem_entry_num; i++) {
		struct mrrm_mem_range_entry *entry;

		entry = mrrm_mem_range_entry + i;

		sprintf(name, "range%d", i);
		entry->dev.init_name = name;

		entry->dev.id = i;
		entry->dev.groups = memory_range_groups;

		ret = memory_subsys_device_register(&entry->dev);
		if (ret) {
			put_device(&entry->dev);
			return ret;
		}
	}

	return ret;
}

static __init int mrrm_init(void)
{
	int ret;

	ret = acpi_table_parse(ACPI_SIG_MRRM, acpi_parse_mrrm);

	if (ret < 0)
		return ret;

	return add_boot_memory_ranges();
}
device_initcall(mrrm_init);
