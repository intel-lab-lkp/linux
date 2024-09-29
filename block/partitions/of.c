// SPDX-License-Identifier: GPL-2.0

#include <linux/blkdev.h>
#include <linux/major.h>
#include <linux/of.h>
#include "check.h"

#define BOOT0_STR	"boot0"
#define BOOT1_STR	"boot1"

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

		if (!memcmp(disk_name + strlen(disk_name) - strlen(BOOT0_STR),
			    BOOT0_STR, sizeof(BOOT0_STR)))
			node_name = "partitions-boot0";
		if (!memcmp(disk_name + strlen(disk_name) - strlen(BOOT1_STR),
			    BOOT1_STR, sizeof(BOOT1_STR)))
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

	/*
	 * Validate offset conversion from bytes to sectors.
	 * Only the first partition is allowed to have offset 0.
	 */
	offset = of_read_number(reg, a_cells);
	if (do_div(offset, SECTOR_SIZE) ||
	    (slot > 1 && !offset))
		return -EINVAL;

	/* Validate size conversion from bytes to sectors */
	size = of_read_number(reg + a_cells, s_cells);
	if (do_div(size, SECTOR_SIZE) || !size)
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
	int slot;

	disk_np = of_node_get(ddev->parent->of_node);
	if (!disk_np)
		return 0;

	partitions_np = get_partitions_node(disk_np, state->disk);
	if (!partitions_np ||
	    !of_device_is_compatible(partitions_np, "fixed-partitions"))
		return 0;

	/* Check if child are over the limit */
	slot = of_get_child_count(partitions_np);
	if (slot >= state->limit)
		goto err;

	slot = 1;
	/* Validate parition offset and size */
	for_each_child_of_node(partitions_np, np) {
		if (validate_of_partition(np, slot))
			goto err;

		slot++;
	}

	slot = 1;
	for_each_child_of_node(partitions_np, np) {
		add_of_partition(state, slot, np);

		slot++;
	}

	strlcat(state->pp_buf, "\n", PAGE_SIZE);

	return 1;
err:
	of_node_put(partitions_np);
	of_node_put(disk_np);
	return -1;
}
