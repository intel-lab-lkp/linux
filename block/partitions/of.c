// SPDX-License-Identifier: GPL-2.0

#include <linux/blkdev.h>
#include <linux/major.h>
#include <linux/of.h>
#include <linux/string.h>
#include "check.h"

static struct device_node *get_partitions_node(struct device_node *disk_np,
					       struct gendisk *disk)
{
	const char *node_name = "partitions";

	/*
	 * JEDEC specification 4.4 for eMMC introduced 3 additional partition
	 * present on every eMMC. These additional partition are always hardcoded
	 * from the eMMC driver as boot0, boot1 and rpmb. While rpmb is used to
	 * store keys and exposed as a char device, the other 2 are exposed as
	 * real separate disk with the boot0/1 appended to the disk name.
	 *
	 * Here we parse the disk_name in search for such suffix and select
	 * the correct partition node.
	 */
	if (disk->major == MMC_BLOCK_MAJOR) {
		const char *disk_name = disk->disk_name;

		if (strends(disk_name, "boot0"))
			node_name = "partitions-boot0";
		if (strends(disk_name, "boot1"))
			node_name = "partitions-boot1";
	}

	return of_get_child_by_name(disk_np, node_name);
}

static int validate_of_partition(struct device_node *np, int slot)
{
	int a_cells, s_cells;
	const __be32 *reg;
	u64 offset, size;
	int len;

	reg = of_get_property(np, "reg", &len);

	a_cells = of_n_addr_cells(np);
	s_cells = of_n_size_cells(np);

	/* Make sure reg len match the expected addr and size cells */
	if (len / sizeof(*reg) != a_cells + s_cells)
		return -EINVAL;

	/* Validate offset conversion from bytes to sectors */
	offset = of_read_number(reg, a_cells);
	if (offset % SECTOR_SIZE)
		return -EINVAL;

	/* Validate size conversion from bytes to sectors */
	size = of_read_number(reg + a_cells, s_cells);
	if (!size || size % SECTOR_SIZE)
		return -EINVAL;

	return 0;
}

static void add_of_partition(struct parsed_partitions *state, int slot,
			     struct device_node *np)
{
	struct partition_meta_info *info;
	char tmp[sizeof(info->volname) + 4];
	int a_cells, s_cells;
	const char *partname;
	const __be32 *reg;
	u64 offset, size;
	int len;

	reg = of_get_property(np, "reg", &len);

	a_cells = of_n_addr_cells(np);
	s_cells = of_n_size_cells(np);

	/* Convert bytes to sector size */
	offset = of_read_number(reg, a_cells) / SECTOR_SIZE;
	size = of_read_number(reg + a_cells, s_cells) / SECTOR_SIZE;

	put_partition(state, slot, offset, size);

	if (of_property_read_bool(np, "read-only"))
		state->parts[slot].flags |= ADDPART_FLAG_READONLY;

	/*
	 * Follow MTD label logic, search for label property,
	 * fallback to node name if not found.
	 */
	info = &state->parts[slot].info;
	partname = of_get_property(np, "label", &len);
	if (!partname)
		partname = of_get_property(np, "name", &len);
	strscpy(info->volname, partname, sizeof(info->volname));

	snprintf(tmp, sizeof(tmp), "(%s)", info->volname);
	strlcat(state->pp_buf, tmp, PAGE_SIZE);
}

int of_partition(struct parsed_partitions *state)
{
	struct device_node *disk_np, *partitions_np, *np;
	struct device *ddev = disk_to_dev(state->disk);
	int slot, ret = 1;

	disk_np = of_node_get(ddev->parent->of_node);
	if (!disk_np)
		return 0;

	partitions_np = get_partitions_node(disk_np, state->disk);
	if (!partitions_np ||
	    !of_device_is_compatible(partitions_np, "fixed-partitions")) {
		of_node_put(disk_np);
		return 0;
	}

	slot = 1;
	/* Validate parition offset and size */
	for_each_child_of_node(partitions_np, np) {
		if (validate_of_partition(np, slot)) {
			of_node_put(np);
			ret = -1;
			goto exit;
		}

		slot++;
	}

	slot = 1;
	for_each_child_of_node(partitions_np, np) {
		if (slot >= state->limit) {
			of_node_put(np);
			break;
		}

		add_of_partition(state, slot, np);

		slot++;
	}

	strlcat(state->pp_buf, "\n", PAGE_SIZE);

exit:
	of_node_put(partitions_np);
	of_node_put(disk_np);
	return ret;
}
