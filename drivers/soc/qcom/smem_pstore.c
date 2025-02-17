// SPDX-License-Identifier: GPL-2.0-only
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/devcoredump.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <linux/dma-mapping.h>
#include <linux/pstore_smem.h>
#include <linux/pstore_zone.h>
#include <linux/pstore.h>

static LIST_HEAD(apss_md_rlist);
struct md_region_list {
	struct qcom_minidump_region md_region;
	struct list_head list;
};

static struct qcom_smem_pstore_context {
	struct pstore_device_info dev;
} oops_ctx;

static int register_smem_region(const char *name, int id, void *vaddr,
				   phys_addr_t paddr, size_t size)
{
	struct qcom_minidump_region *md_region;
	int ret;

	struct md_region_list *mdr_list =
		kzalloc(sizeof(*mdr_list), GFP_KERNEL);
	if (!mdr_list)
		return -ENOMEM;
	md_region = &mdr_list->md_region;

	scnprintf(md_region->name, sizeof(md_region->name), "K%d%.8s", id, name);
	md_region->virt_addr = vaddr;
	md_region->phys_addr = paddr;
	md_region->size = size;
	ret = qcom_minidump_region_register(md_region);
	if (ret < 0) {
		pr_err("failed to register region in minidump: err: %d\n", ret);
		return ret;
	}

	list_add(&mdr_list->list, &apss_md_rlist);
	return 0;
}

static int unregister_smem_region(void *vaddr,
					phys_addr_t paddr, size_t size)
{
	int ret = -ENOENT;
	struct md_region_list *mdr_list;
	struct md_region_list *tmp;

	list_for_each_entry_safe(mdr_list, tmp, &apss_md_rlist, list) {
		struct qcom_minidump_region *region;

		region = &mdr_list->md_region;
		if (region->virt_addr == vaddr) {
			ret = qcom_minidump_region_unregister(region);
			list_del(&mdr_list->list);
			goto unregister_smem_region_exit;
		}
	}

unregister_smem_region_exit:
	pr_err("failed to unregister region in minidump: err: %d\n", ret);

	return ret;
}

static int qcom_smem_register_dmr(char *name, int id, void *area, size_t size)
{
	return register_smem_region(name, id, area, virt_to_phys(area), size);
}

static int qcom_smem_unregister_dmr(void *area, size_t size)
{
	return unregister_smem_region(area, virt_to_phys(area), size);
}

int qcom_register_pstore_smem(struct device *dev)
{
	int ret;

	struct qcom_smem_pstore_context *ctx = &oops_ctx;

	ctx->dev.flags = PSTORE_FLAGS_DMAPPED;
	ctx->dev.zone.register_dmr = qcom_smem_register_dmr;
	ctx->dev.zone.unregister_dmr = qcom_smem_unregister_dmr;
	ctx->dev.zone.dmapped_cnt = 2;

	ret = register_pstore_smem_device(&ctx->dev);
	if (ret)
		dev_warn(dev, "Could not register pstore smem device.");

	return 0;
}

void qcom_unregister_pstore_smem(void)
{
	struct qcom_smem_pstore_context *ctx = &oops_ctx;

	unregister_pstore_smem_device(&ctx->dev);
}
