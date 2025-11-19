// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Minidump kernel inspect driver
 * Copyright (C) 2016,2024-2025 Linaro Ltd
 * Copyright (C) 2015 Sony Mobile Communications Inc
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/minidump.h>
#include <linux/meminspect.h>

/**
 * struct minidump - Minidump driver data information
 *
 * @dev:	Minidump device struct.
 * @toc:	Minidump table of contents subsystem.
 * @regions:	Minidump regions array.
 * @nb:		Notifier block to register to meminspect.
 */
struct minidump {
	struct device			*dev;
	struct minidump_subsystem	*toc;
	struct minidump_region		*regions;
	struct notifier_block		nb;
};

static const char * const meminspect_id_to_md_string[] = {
	"",
	"ELF",
	"vmcoreinfo",
	"config",
	"totalram",
	"cpu_possible",
	"cpu_present",
	"cpu_online",
	"cpu_active",
	"mem_section",
	"jiffies",
	"linux_banner",
	"nr_threads",
	"nr_irqs",
	"tainted_mask",
	"taint_flags",
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
	"high_memory",
	"init_mm",
	"init_mm_pgd",
};

/**
 * qcom_md_table_init() - Initialize the minidump table
 * @md: minidump data
 * @mdss_toc: minidump subsystem table of contents
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int qcom_md_table_init(struct minidump *md,
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
 * qcom_md_get_region_index() - Lookup minidump region by id
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
 * @priv: private data
 * @e: pointer to inspect entry
 *
 * Return: None
 */
static void __maybe_unused register_md_region(void *priv,
					      const struct inspect_entry *e)
{
	unsigned int num_region, region_cnt;
	const char *name = "unknown";
	struct minidump_region *mdr;
	struct minidump *md = priv;

	if (!(e->va || e->pa) || !e->size) {
		dev_dbg(md->dev, "invalid region requested\n");
		return;
	}

	if (e->id < ARRAY_SIZE(meminspect_id_to_md_string))
		name = meminspect_id_to_md_string[e->id];

	if (qcom_md_get_region_index(md, e->id) >= 0) {
		dev_dbg(md->dev, "%s:%d region is already registered\n",
			name, e->id);
		return;
	}

	/* Check if there is a room for a new entry */
	num_region = le32_to_cpu(md->toc->region_count);
	if (num_region >= MAX_NUM_REGIONS) {
		dev_dbg(md->dev, "maximum region limit %u reached\n",
			num_region);
		return;
	}

	region_cnt = le32_to_cpu(md->toc->region_count);
	mdr = &md->regions[region_cnt];
	scnprintf(mdr->name, MAX_REGION_NAME_LENGTH, "K%.8s", name);
	mdr->seq_num = e->id;
	if (e->pa)
		mdr->address = cpu_to_le64(e->pa);
	else if (e->va)
		mdr->address = cpu_to_le64(__pa(e->va));
	mdr->size = cpu_to_le64(ALIGN(e->size, 4));
	mdr->valid = cpu_to_le32(MINIDUMP_REGION_VALID);
	region_cnt++;
	md->toc->region_count = cpu_to_le32(region_cnt);

	dev_dbg(md->dev, "%s:%d region registered %llx:%llx\n",
		mdr->name, mdr->seq_num, mdr->address, mdr->size);
}

/**
 * unregister_md_region() - Unregister a previously registered minidump region
 * @priv: private data
 * @e: pointer to inspect entry
 *
 * Return: None
 */
static void __maybe_unused unregister_md_region(void *priv,
						const struct inspect_entry *e)
{
	struct minidump_region *mdr;
	struct minidump *md = priv;
	unsigned int region_cnt;
	unsigned int idx;

	idx = qcom_md_get_region_index(md, e->id);
	if (idx < 0) {
		dev_dbg(md->dev, "%d region is not present\n", e->id);
		return;
	}

	mdr = &md->regions[0];
	region_cnt = le32_to_cpu(md->toc->region_count);

	/*
	 * Left shift one position all the regions located after the
	 * region being removed, in order to fill the gap.
	 * Then, zero out the last region at the end.
	 */
	memmove(&mdr[idx], &mdr[idx + 1], (region_cnt - idx - 1) * sizeof(*mdr));
	memset(&mdr[region_cnt - 1], 0, sizeof(*mdr));
	region_cnt--;
	md->toc->region_count = cpu_to_le32(region_cnt);
}

static int qcom_md_notifier_cb(struct notifier_block *nb,
			       unsigned long code, void *entry)
{
	struct minidump *md = container_of(nb, struct minidump, nb);

	if (code == MEMINSPECT_NOTIFIER_ADD)
		register_md_region(md, entry);
	else if (code == MEMINSPECT_NOTIFIER_REMOVE)
		unregister_md_region(md, entry);

	return 0;
}

static int qcom_md_probe(struct platform_device *pdev)
{
	struct minidump_global_toc *mdgtoc;
	struct device *dev = &pdev->dev;
	struct minidump *md;
	size_t size;
	int ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	platform_set_drvdata(pdev, md);

	md->dev = dev;
	md->nb.notifier_call = qcom_md_notifier_cb;

	mdgtoc = qcom_smem_get(QCOM_SMEM_HOST_ANY, SBL_MINIDUMP_SMEM_ID, &size);
	if (IS_ERR(mdgtoc)) {
		ret = PTR_ERR(mdgtoc);
		dev_err_probe(dev, ret, "Couldn't find minidump smem item\n");
	}

	if (size < sizeof(*mdgtoc) || !mdgtoc->status)
		dev_err_probe(dev, -EINVAL, "minidump table not ready\n");

	ret = qcom_md_table_init(md, &mdgtoc->subsystems[MINIDUMP_SUBSYSTEM_APSS]);
	if (ret)
		dev_err_probe(dev, ret, "Could not initialize table\n");

	meminspect_notifier_register(&md->nb);

	meminspect_lock_traverse(md, register_md_region);
	return 0;
}

static void qcom_md_remove(struct platform_device *pdev)
{
	struct minidump *md = platform_get_drvdata(pdev);

	meminspect_notifier_unregister(&md->nb);
	meminspect_lock_traverse(md, unregister_md_region);
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
MODULE_DESCRIPTION("Qualcomm minidump inspect driver");
MODULE_LICENSE("GPL");
