// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Minidump backend driver for Kmemdump
 * Copyright (C) 2016,2024-2025 Linaro Ltd
 * Copyright (C) 2015 Sony Mobile Communications Inc
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/kmemdump.h>
#include <linux/container_of.h>

/*
 * In some of the Old Qualcomm devices, boot firmware statically allocates 300
 * as total number of supported region (including all co-processors) in
 * minidump table out of which linux was using 201. In future, this limitation
 * from boot firmware might get removed by allocating the region dynamically.
 * So, keep it compatible with older devices, we can keep the current limit for
 * Linux to 201.
 */
#define MAX_NUM_REGIONS		201

#define MAX_NUM_SUBSYSTEMS	10
#define MAX_REGION_NAME_LENGTH	16
#define SBL_MINIDUMP_SMEM_ID	602
#define MINIDUMP_REGION_VALID	('V' << 24 | 'A' << 16 | 'L' << 8 | 'I' << 0)
#define MINIDUMP_SS_ENCR_DONE	('D' << 24 | 'O' << 16 | 'N' << 8 | 'E' << 0)
#define MINIDUMP_SS_ENABLED	('E' << 24 | 'N' << 16 | 'B' << 8 | 'L' << 0)

#define MINIDUMP_SS_ENCR_NOTREQ	(0 << 24 | 0 << 16 | 'N' << 8 | 'R' << 0)

#define MINIDUMP_SUBSYSTEM_APSS	0

const char *kmemdump_id_to_md_string[] = {
	"",
	"ELF",
	"vmcoreinfo",
	"config",
	"memsect",
	"totalram",
	"cpu_possible",
	"cpu_present",
	"cpu_online",
	"cpu_active",
	"jiffies",
	"linux_banner",
	"nr_threads",
	"nr_irqs",
	"tainted_mask",
	"taint_flags",
	"mem_section",
	"node_data",
	"node_states",
	"__per_cpu_offset",
	"nr_swapfiles",
	"init_uts_ns",
	"printk_rb_static",
	"printk_rb_dynamic",
	"prb",
	"prb_descs",
	"prb_infos",
	"prb_data",
	"runqueues",
	"high_memory",
	"init_mm",
	"init_mm_pgd",
};

/**
 * struct minidump_region - Minidump region
 * @name		: Name of the region to be dumped
 * @seq_num:		: Use to differentiate regions with same name.
 * @valid		: This entry to be dumped (if set to 1)
 * @address		: Physical address of region to be dumped
 * @size		: Size of the region
 */
struct minidump_region {
	char	name[MAX_REGION_NAME_LENGTH];
	__le32	seq_num;
	__le32	valid;
	__le64	address;
	__le64	size;
};

/**
 * struct minidump_subsystem - Subsystem's SMEM Table of content
 * @status : Subsystem toc init status
 * @enabled : if set to 1, this region would be copied during coredump
 * @encryption_status: Encryption status for this subsystem
 * @encryption_required : Decides to encrypt the subsystem regions or not
 * @region_count : Number of regions added in this subsystem toc
 * @regions_baseptr : regions base pointer of the subsystem
 */
struct minidump_subsystem {
	__le32	status;
	__le32	enabled;
	__le32	encryption_status;
	__le32	encryption_required;
	__le32	region_count;
	__le64	regions_baseptr;
};

/**
 * struct minidump_global_toc - Global Table of Content
 * @status : Global Minidump init status
 * @revision : Minidump revision
 * @enabled : Minidump enable status
 * @subsystems : Array of subsystems toc
 */
struct minidump_global_toc {
	__le32				status;
	__le32				revision;
	__le32				enabled;
	struct minidump_subsystem	subsystems[MAX_NUM_SUBSYSTEMS];
};

#define MINIDUMP_MAX_NAME_LENGTH	12
/**
 * struct qcom_minidump_region - Minidump region information
 *
 * @name:	Minidump region name
 * @virt_addr:  Virtual address of the entry.
 * @phys_addr:	Physical address of the entry to dump.
 * @size:	Number of bytes to dump from @address location,
 *		and it should be 4 byte aligned.
 * @id:		Region id.
 */
struct qcom_minidump_region {
	char		name[MINIDUMP_MAX_NAME_LENGTH];
	void		*virt_addr;
	phys_addr_t	phys_addr;
	size_t		size;
	unsigned int	id;
};

/**
 * struct minidump - Minidump driver data information
 *
 * @dev:	Minidump device struct.
 * @toc:	Minidump table of contents subsystem.
 * @regions:	Minidump regions array.
 * @md_be:	Minidump backend.
 */
struct minidump {
	struct device			*dev;
	struct minidump_subsystem	*toc;
	struct minidump_region		*regions;
	struct kmemdump_backend		md_be;
};

static struct minidump *md;

#define be_to_minidump(be) container_of(be, struct minidump, md_be)

/**
 * qcom_apss_md_table_init() - Initialize the minidump table
 * @md: minidump data
 * @mdss_toc: minidump subsystem table of contents
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int qcom_apss_md_table_init(struct minidump *md,
				   struct minidump_subsystem *mdss_toc)
{
	md->toc = mdss_toc;
	md->regions = devm_kcalloc(md->dev, MAX_NUM_REGIONS,
				   sizeof(*md->regions), GFP_KERNEL);
	if (!md->regions)
		return -ENOMEM;

	md->toc->regions_baseptr = cpu_to_le64(virt_to_phys(md->regions));
	md->toc->enabled = cpu_to_le32(MINIDUMP_SS_ENABLED);
	md->toc->status = cpu_to_le32(1);
	md->toc->region_count = cpu_to_le32(0);

	/* Tell bootloader not to encrypt the regions of this subsystem */
	md->toc->encryption_status = cpu_to_le32(MINIDUMP_SS_ENCR_DONE);
	md->toc->encryption_required = cpu_to_le32(MINIDUMP_SS_ENCR_NOTREQ);

	return 0;
}

/**
 * qcom_md_get_region_index() - Lookup minidump region by kmemdump id
 * @md: minidump data
 * @id: minidump region id
 *
 * Return: On success, it returns the internal region index, on failure,
 *	returns	negative error value
 */
static int qcom_md_get_region_index(struct minidump *md, int id)
{
	unsigned int count = le32_to_cpu(md->toc->region_count);
	unsigned int i;

	for (i = 0; i < count; i++)
		if (md->regions[i].seq_num == id)
			return i;

	return -ENOENT;
}

/**
 * register_md_region() - Register a new minidump region
 * @be: kmemdump backend, this should be the minidump backend
 * @id: unique id to identify the region
 * @vaddr: virtual memory address of the region start
 * @size: size of the region
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int register_md_region(const struct kmemdump_backend *be,
			      enum kmemdump_uid id, void *vaddr, size_t size)
{
	struct minidump *md = be_to_minidump(be);
	struct minidump_region *mdr;
	unsigned int num_region, region_cnt;
	const char *name = "unknown";

	if (!vaddr || !size)
		return -EINVAL;

	if (id < ARRAY_SIZE(kmemdump_id_to_md_string))
		name = kmemdump_id_to_md_string[id];

	if (qcom_md_get_region_index(md, id) >= 0) {
		dev_dbg(md->dev, "%s:%d region is already registered\n",
			name, id);
		return -EEXIST;
	}

	/* Check if there is a room for a new entry */
	num_region = le32_to_cpu(md->toc->region_count);
	if (num_region >= MAX_NUM_REGIONS) {
		dev_err(md->dev, "maximum region limit %u reached\n",
			num_region);
		return -ENOSPC;
	}

	region_cnt = le32_to_cpu(md->toc->region_count);
	mdr = &md->regions[region_cnt];
	scnprintf(mdr->name, MAX_REGION_NAME_LENGTH, "K%.8s", name);
	mdr->seq_num = id;
	mdr->address = cpu_to_le64(__pa(vaddr));
	mdr->size = cpu_to_le64(ALIGN(size, 4));
	mdr->valid = cpu_to_le32(MINIDUMP_REGION_VALID);
	region_cnt++;
	md->toc->region_count = cpu_to_le32(region_cnt);

	return 0;
}

/**
 * unregister_md_region() - Unregister a previously registered minidump region
 * @be: pointer to backend
 * @id: unique id to identify the region
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int unregister_md_region(const struct kmemdump_backend *be,
				unsigned int id)
{
	struct minidump *md = be_to_minidump(be);
	struct minidump_region *mdr;
	unsigned int region_cnt;
	unsigned int idx;

	idx = qcom_md_get_region_index(md, id);
	if (idx < 0) {
		dev_err(md->dev, "%d region is not present\n", id);
		return idx;
	}

	mdr = &md->regions[0];
	region_cnt = le32_to_cpu(md->toc->region_count);
	/*
	 * Left shift all the regions exist after this removed region
	 * index by 1 to fill the gap and zero out the last region
	 * present at the end.
	 */
	memmove(&mdr[idx], &mdr[idx + 1], (region_cnt - idx - 1) * sizeof(*mdr));
	memset(&mdr[region_cnt - 1], 0, sizeof(*mdr));
	region_cnt--;
	md->toc->region_count = cpu_to_le32(region_cnt);

	return 0;
}

static int qcom_md_probe(struct platform_device *pdev)
{
	struct minidump_global_toc *mdgtoc;
	size_t size;
	int ret;

	md = kzalloc(sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;

	md->dev = &pdev->dev;

	strscpy(md->md_be.name, "qcom_minidump");
	md->md_be.register_region = register_md_region;
	md->md_be.unregister_region = unregister_md_region;

	mdgtoc = qcom_smem_get(QCOM_SMEM_HOST_ANY, SBL_MINIDUMP_SMEM_ID, &size);
	if (IS_ERR(mdgtoc)) {
		ret = PTR_ERR(mdgtoc);
		dev_err(md->dev, "Couldn't find minidump smem item %d\n", ret);
		goto qcom_md_probe_fail;
	}

	if (size < sizeof(*mdgtoc) || !mdgtoc->status) {
		dev_err(md->dev, "minidump table is not initialized %d\n", ret);
		ret = -ENAVAIL;
		goto qcom_md_probe_fail;
	}

	ret = qcom_apss_md_table_init(md, &mdgtoc->subsystems[MINIDUMP_SUBSYSTEM_APSS]);
	if (ret)
		goto qcom_md_probe_fail;

	return kmemdump_register_backend(&md->md_be);

qcom_md_probe_fail:
	kfree(md);
	return ret;
}

static void qcom_md_remove(struct platform_device *pdev)
{
	kfree(md);
	kmemdump_unregister_backend(&md->md_be);
}

static struct platform_driver qcom_md_driver = {
	.probe = qcom_md_probe,
	.remove = qcom_md_remove,
	.driver  = {
		.name = "qcom-minidump",
	},
};

module_platform_driver(qcom_md_driver);

MODULE_AUTHOR("Eugen Hristev <eugen.hristev@linaro.org>");
MODULE_AUTHOR("Mukesh Ojha <quic_mojha@quicinc.com>");
MODULE_DESCRIPTION("Qualcomm kmemdump minidump backend driver");
MODULE_LICENSE("GPL");
