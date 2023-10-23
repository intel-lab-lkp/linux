// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Qualcomm memory dump driver dynamically reserves memory and provides
 * hints(id and size) of debugging information based on specified
 * protocols with firmware into pre-allocated memory. Firmware then does the
 * real data capture. The debugging information includes cache contents,
 * internal memory, registers.
 * After crash and warm reboot, firmware scans ids, sizes and stores contents
 * into reserved memory accordingly. Firmware then enters into full dump mode
 * which dumps whole DDR to host through USB.
 *
 */
#include <asm/barrier.h>
#include <linux/bootconfig.h>
#include <linux/cma.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#define MAX_NUM_ENTRIES		0x150
#define QCOM_DUMP_MAKE_VERSION(major, minor)	(((major) << 20) | (minor))
#define QCOM_DUMP_TABLE_VERSION		QCOM_DUMP_MAKE_VERSION(2, 0)
#define QCOM_DUMP_DATA_SIZE sizeof(struct qcom_dump_data)

enum qcom_dump_table_ids {
	QCOM_DUMP_TABLE_LINUX,
	QCOM_DUMP_TABLE_MAX = MAX_NUM_ENTRIES,
};

enum qcom_dump_type {
	QCOM_DUMP_TYPE_DATA,
	QCOM_DUMP_TYPE_TABLE,
};

/*
 * +----------+         1st level
 * |IMEM      |------+-----------------+
 * +----------+      | qcom_dump_table |
 *                   |---------------- |
 *                   | version         |
 *                   | num_entryies    |
 *                   | ..              |
 *                   |---------------- |
 *             +-----|qcom_dump_entry  |
 *             |     |qcom_dump_entry  |
 *             |     |  ...            |
 *             |     +-----------------+
 *             |
 *             |
 *             |        2nd level
 *             |     +-----------------+
 *             ------| qcom_dump_table |
 *                   |---------------- |
 *                   | version         |
 *                   | num_entryies    |
 *                   | ..              |
 *                   |---------------- |     +-------------+     +----------+
 *                   |qcom_dump_entry  |-----|qcom_dump_data|----| data     |
 *                   |qcom_dump_entry  |     +-------------+     +----------+
 *                   |  ...            |---- +-------------+     +----------+
 *                   +-----------------+     |qcom_dump_data|----| data     |
 *                                           +-------------+     +----------+
 *
 * Structures can not be packed due to protocols with firmware.
 */
struct qcom_dump_data {
	__le32 version;
	__le32 magic;
	char name[32];
	__le64 addr;
	__le64 len;
	__le32 reserved;
};

struct qcom_dump_entry {
	__le32 id;
	char name[32];
	__le32 type;
	__le64 addr;
};

struct qcom_dump_table {
	__le32 version;
	__le32 num_entries;
	struct qcom_dump_entry entries[MAX_NUM_ENTRIES];
};

struct qcom_memory_dump {
	u64 table_phys;
	struct qcom_dump_table *table;
	struct xbc_node *mem_dump_node;
	/* Cached 2nd level table */
	struct qcom_dump_table *cached_2nd_table;
};

static void __init mem_dump_entry_set(struct device *dev,
				      struct qcom_dump_entry *entry,
				      u32 id,
				      u32 type, uint64_t addr)
{
	entry->id = id;
	entry->type = type;
	entry->addr = addr;
}

/* 1st level table register */
static int __init mem_dump_table_register(struct device *dev,
					  struct qcom_dump_entry *entry)
{
	struct qcom_memory_dump *memdump = dev_get_drvdata(dev);
	struct qcom_dump_entry *last_entry;
	struct qcom_dump_table *table = memdump->table;

	if (!table || table->num_entries >= MAX_NUM_ENTRIES)
		return -EINVAL;

	last_entry = &table->entries[table->num_entries];
	mem_dump_entry_set(dev, last_entry, entry->id,
			   QCOM_DUMP_TYPE_TABLE, entry->addr);
	table->num_entries++;

	return 0;
}

/* Get 2nd level table */
static struct qcom_dump_table * __init
mem_dump_get_table(struct device *dev,
		   enum qcom_dump_table_ids id)
{
	struct qcom_memory_dump *memdump = dev_get_drvdata(dev);
	struct qcom_dump_table *table = memdump->table;
	unsigned long offset;
	int i;

	if (memdump->cached_2nd_table)
		return memdump->cached_2nd_table;

	if (!table) {
		dev_err(dev, "Mem dump base table does not exist\n");
		return ERR_PTR(-EINVAL);
	}

	for (i = 0; i < MAX_NUM_ENTRIES; i++) {
		if (table->entries[i].id == id)
			break;
	}

	if (i == MAX_NUM_ENTRIES || !table->entries[i].addr) {
		dev_err(dev, "Mem dump base table entry %d invalid\n", id);
		return ERR_PTR(-EINVAL);
	}

	offset = table->entries[i].addr - memdump->table_phys;

	/* Get the table pointer. Phy and virt addr has same offset */
	table = (void *)memdump->table + offset;
	/* Cache it for next time visit */
	memdump->cached_2nd_table = table;

	return table;
}

/* register in 2nd level table */
static int __init mem_dump_data_register(struct device *dev,
					 enum qcom_dump_table_ids id,
					 struct qcom_dump_entry *entry)
{
	struct qcom_dump_entry *last_entry;
	struct qcom_dump_table *table;

	/* Get 2nd level table */
	table = mem_dump_get_table(dev, id);
	if (IS_ERR(table))
		return PTR_ERR(table);

	if (!table || table->num_entries >= MAX_NUM_ENTRIES)
		return -EINVAL;

	last_entry = &table->entries[table->num_entries];
	mem_dump_entry_set(dev, last_entry, entry->id, QCOM_DUMP_TYPE_DATA,
			   entry->addr);
	table->num_entries++;

	return 0;
}

static int __init qcom_init_memdump_imem_area(struct device *dev, size_t size)
{
	struct qcom_memory_dump *memdump = dev_get_drvdata(dev);
	void __iomem *table_offset;
	void __iomem *table_base;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL,
				     "qcom,qcom-imem-mem-dump-table");
	if (!np) {
		dev_err_probe(dev, -ENODEV,
			      "Mem dump base table DT node does not exist\n");
		return -ENODEV;
	}

	table_base = devm_of_iomap(dev, np, 0, NULL);
	if (!table_base) {
		dev_err_probe(dev, -ENOMEM,
			      "Mem dump base table imem offset mapping failed\n");
		return -ENOMEM;
	}

	np = of_find_compatible_node(NULL, NULL,
				     "qcom,qcom-imem-mem-dump-table-size");
	if (!np) {
		dev_err_probe(dev, -ENODEV,
			      "Mem dump base table size DT node does not exist\n");
		devm_iounmap(dev, table_base);
		return -ENODEV;
	}

	table_offset = devm_of_iomap(dev, np, 0, NULL);
	if (!table_offset) {
		dev_err_probe(dev, -ENOMEM,
			      "Mem dump base table size imem offset mapping failed\n");
		devm_iounmap(dev, table_base);
		return -ENOMEM;
	}

	memcpy_toio(table_base, &memdump->table_phys,
		    sizeof(memdump->table_phys));
	memcpy_toio(table_offset,
		    &size, sizeof(size_t));

	/* Ensure write to table_base is complete before unmapping */
	mb();
	dev_dbg(dev, "QCOM Memory Dump base table set up in IMEM\n");

	devm_iounmap(dev, table_base);
	devm_iounmap(dev, table_offset);
	return 0;
}

/* Helper function for applying both vaddr and phys addr */
static void __init mem_dump_apply_offset(void **dump_vaddr,
					 phys_addr_t *phys_addr, size_t offset)
{
	*dump_vaddr += offset;
	*phys_addr += offset;
}

/* Populate 1st level: QCOM_DUMP_TABLE_LINUX */
static int __init mem_dump_register_data_table(struct device *dev,
					       void *dump_vaddr,
					       phys_addr_t phys_addr)
{
	struct qcom_memory_dump *memdump = dev_get_drvdata(dev);
	struct qcom_dump_table *table;
	struct qcom_dump_entry entry;
	int ret;

	memdump->table = dump_vaddr;
	memdump->table->version = QCOM_DUMP_TABLE_VERSION;
	memdump->table_phys = phys_addr;
	mem_dump_apply_offset(&dump_vaddr, &phys_addr, sizeof(*table));

	table = dump_vaddr;
	table->version = QCOM_DUMP_TABLE_VERSION;
	entry.id = QCOM_DUMP_TABLE_LINUX;
	entry.addr = phys_addr;
	ret = mem_dump_table_register(dev, &entry);
	if (ret) {
		dev_err(dev, "Mem dump apps data table register failed\n");
		return ret;
	}

	return 0;
}

static int __init mem_dump_reserve_mem(struct device *dev)
{
	int ret;

	if (of_property_present(dev->of_node, "memory-region")) {
		ret = of_reserved_mem_device_init_by_idx(dev,
							 dev->of_node, 0);
		if (ret)
			dev_err_probe(dev, ret,
				      "Failed to initialize reserved mem\n");
		return ret;
	}

	/* Using default CMA region is fallback choice */
	dev_dbg(dev, "Using default CMA region\n");
	return 0;
}

static struct page * __init
mem_dump_alloc_mem(struct device *dev, size_t *total_size)
{
	struct qcom_memory_dump *memdump = dev_get_drvdata(dev);
	struct xbc_node *linked_list;
	int num_of_nodes = 0;
	struct page *page;
	const char *size_p;
	const char *id_p;
	int ret = 0;
	int size;
	int id;

	memdump->mem_dump_node = xbc_find_node("memory_dump_config");
	if (!memdump->mem_dump_node) {
		dev_err(dev, "xbc config not found\n");
		return ERR_PTR(-EINVAL);
	}

	*total_size = sizeof(struct qcom_dump_table) * 2;

	xbc_node_for_each_subkey(memdump->mem_dump_node, linked_list) {
		const char *name = xbc_node_get_data(linked_list);

		if (!name)
			continue;

		id_p = xbc_node_find_value(linked_list, "id", NULL);
		size_p = xbc_node_find_value(linked_list, "size", NULL);

		if (id_p && size_p) {
			ret = kstrtoint(id_p, 0, &id);
			if (ret)
				continue;

			ret = kstrtoint(size_p, 0, &size);
			if (ret)
				continue;

			if (check_add_overflow(*total_size, size, total_size))
				return ERR_PTR(-EOVERFLOW);

			num_of_nodes++;
		} else {
			continue;
		}
	}

	if (!num_of_nodes)
		return ERR_PTR(-EINVAL);

	if (check_add_overflow(*total_size,
			       (QCOM_DUMP_DATA_SIZE * num_of_nodes),
			       total_size))
		return ERR_PTR(-EOVERFLOW);

	/* Align total_size */
	if (*total_size > ALIGN(*total_size, PAGE_SIZE))
		return ERR_PTR(-EOVERFLOW);
	*total_size = ALIGN(*total_size, PAGE_SIZE);

	/*
	 * Physical continuous buffer.
	 */
	page = cma_alloc(dev_get_cma_area(dev), (*total_size / PAGE_SIZE),
			 0, false);
	if (page)
		memset(page_address(page), 0, *total_size);
	else
		return ERR_PTR(-ENOMEM);

	return page;
}

/* populate allocated region */
static int __init mem_dump_populate_mem(struct device *dev,
					struct page *start_page,
					size_t total_size)
{
	struct qcom_memory_dump *memdump = dev_get_drvdata(dev);
	struct qcom_dump_entry dump_entry;
	struct qcom_dump_data *dump_data;
	struct xbc_node *linked_list;
	phys_addr_t phys_end_addr;
	phys_addr_t phys_addr;
	const char *size_p;
	void *dump_vaddr;
	const char *id_p;
	int ret = 0;
	int size;
	int id;

	phys_addr = page_to_phys(start_page);
	phys_end_addr = phys_addr + total_size;
	dump_vaddr = page_to_virt(start_page);

	ret = mem_dump_register_data_table(dev, dump_vaddr, phys_addr);
	if (ret) {
		dev_err_probe(dev, ret, "Mem Dump table set up is failed\n");
		return ret;
	}

	ret = qcom_init_memdump_imem_area(dev, total_size);
	if (ret)
		return ret;

	/* Apply two tables: QCOM_DUMP_TYPE_TABLE and QCOM_DUMP_TYPE_DATA */
	mem_dump_apply_offset(&dump_vaddr, &phys_addr,
			      sizeof(struct qcom_dump_table) * 2);

	/* Both "id" and "size" must be present */
	xbc_node_for_each_subkey(memdump->mem_dump_node, linked_list) {
		const char *name = xbc_node_get_data(linked_list);

		if (!name)
			continue;

		id_p = xbc_node_find_value(linked_list, "id", NULL);
		size_p = xbc_node_find_value(linked_list, "size", NULL);

		if (id_p && size_p) {
			ret = kstrtoint(id_p, 0, &id);
			if (ret)
				continue;

			ret = kstrtoint(size_p, 0, &size);

			if (ret)
				continue;

		/*
		 * Physical layout: starting from two qcom_dump_data.
		 * Following are respective dump meta data and reserved regions.
		 * Qcom_dump_data is populated by the driver, fw parse it
		 * and dump respective info into dump mem.
		 * Illustrate the layout:
		 *
		 *   +------------------------+------------------------+
		 *   | qcom_dump_table(TABLE) | qcom_dump_table(DATA)  |
		 *   +------------------------+------------------------+
		 *   +-------------+----------+-------------+----------+
		 *   |qcom_dump_data| dump mem|qcom_dump_data| dump mem |
		 *   +-------------+----------+-------------+----------+
		 *   +-------------+----------+-------------+----------+
		 *   |qcom_dump_data| dump mem|qcom_dump_data| dump mem |
		 *   +-------------+----------+-------------+----------+
		 *   ...
		 */
			dump_data = dump_vaddr;
			dump_data->addr = phys_addr + QCOM_DUMP_DATA_SIZE;
			dump_data->len = size;
			dump_entry.id = id;
			strscpy(dump_data->name, name,
				sizeof(dump_data->name));
			dump_entry.addr = phys_addr;
			ret = mem_dump_data_register(dev, QCOM_DUMP_TABLE_LINUX,
						     &dump_entry);
			if (ret) {
				dev_err_probe(dev, ret, "Dump data setup failed, id = %d\n",
					      id);
				return ret;
			}

			mem_dump_apply_offset(&dump_vaddr, &phys_addr,
					      size + QCOM_DUMP_DATA_SIZE);
			if (phys_addr > phys_end_addr) {
				dev_err_probe(dev, -ENOMEM, "Exceeding allocated region\n");
				return -ENOMEM;
			}
		} else {
			continue;
		}
	}

	return ret;
}

static int __init mem_dump_probe(struct platform_device *pdev)
{
	struct qcom_memory_dump *memdump;
	struct device *dev = &pdev->dev;
	struct page *page;
	size_t total_size;
	int ret = 0;

	memdump = devm_kzalloc(dev, sizeof(struct qcom_memory_dump),
			       GFP_KERNEL);
	if (!memdump)
		return -ENOMEM;

	dev_set_drvdata(dev, memdump);

	/* check and initiate CMA region */
	ret = mem_dump_reserve_mem(dev);
	if (ret)
		return ret;

	/* allocate and populate */
	page = mem_dump_alloc_mem(dev, &total_size);
	if (IS_ERR(page)) {
		ret = PTR_ERR(page);
		dev_err_probe(dev, ret, "mem dump alloc failed\n");
		goto release;
	}

	ret = mem_dump_populate_mem(dev, page, total_size);
	if (!ret)
		dev_info(dev, "Mem dump region populated successfully\n");
	else
		goto free;

	return 0;

free:
	cma_release(dev_get_cma_area(dev), page, (total_size / PAGE_SIZE));

release:
	of_reserved_mem_device_release(dev);
	return ret;
}

static const struct of_device_id mem_dump_match_table[] = {
	{.compatible = "qcom,mem-dump",},
	{}
};

static struct platform_driver mem_dump_driver = {
	.driver = {
		.name = "qcom_mem_dump",
		.of_match_table = mem_dump_match_table,
	},
};
module_platform_driver_probe(mem_dump_driver, mem_dump_probe);

MODULE_DESCRIPTION("Memory Dump Driver");
MODULE_LICENSE("GPL");
