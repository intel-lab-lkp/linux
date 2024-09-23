// SPDX-License-Identifier: GPL-2.0

#include <linux/blkdev.h>
#include <linux/of.h>
#include "check.h"

#define BOOT0_STR	"boot0"
#define BOOT1_STR	"boot1"

static struct device_node *get_partitions_node(struct device_node *disk_np,
					       const char *disk_name)
{
	const char *node_name = "partitions";

	/* Check if we are parsing boot0 or boot1 */
	if (!memcmp(disk_name + strlen(disk_name) - strlen(BOOT0_STR),
		    BOOT0_STR, sizeof(BOOT0_STR)))
		node_name = "partitions-boot0";
	if (!memcmp(disk_name + strlen(disk_name) - strlen(BOOT1_STR),
		    BOOT1_STR, sizeof(BOOT1_STR)))
		node_name = "partitions-boot1";

	return of_get_child_by_name(disk_np, node_name);
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

	offset = of_read_number(reg, a_cells);
	size = of_read_number(reg + a_cells, s_cells);

	put_partition(state, slot, offset, size);

	if (of_property_read_bool(pp, "read-only"))
		state->parts[slot].flags |= ADDPART_FLAG_READONLY;

	info = &state->parts[slot].info;
	partname = of_get_property(np, "label", &len);
	strscpy(info->volname, partname, sizeof(info->volname));

	snprintf(tmp, sizeof(tmp), "(%s)", info->volname);
	strlcat(state->pp_buf, tmp, PAGE_SIZE);
}

int of_partition(struct parsed_partitions *state)
{
	struct device_node *disk_np, *partitions_np, *np;
	struct device *ddev = disk_to_dev(state->disk);
	int slot = 1;

	disk_np = of_node_get(ddev->parent->of_node);
	if (!disk_np)
		return 0;

	partitions_np = get_partitions_node(disk_np,
					    state->disk->disk_name);
	if (!partitions_np)
		return 0;

	for_each_child_of_node(partitions_np, np) {
		if (slot >= state->limit)
			return -1;

		add_of_partition(state, slot, np);

		slot++;
	}

	strlcat(state->pp_buf, "\n", PAGE_SIZE);

	return 1;
}
