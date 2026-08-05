// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited.
 */

#include <linux/acpi.h>
#include <linux/edac.h>
#include <linux/init.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include "edac_module.h"

#define ECC_CS_COUNT_REG	0x18
#define LOONGSON_EDAC_DSM_UUID	"65d0431b-7eb8-46df-b914-b7d568553140"
#define LOONGSON_EDAC_DSM_FUNC_GET_DIMM_SIZE	1

struct loongson_edac_pvt {
	void __iomem *ecc_base;

	/*
	 * The ECC register in this controller records the number of errors
	 * encountered since reset and cannot be zeroed so in order to be able
	 * to report the error count at each check, this records the previous
	 * register state.
	 */
	int last_ce_count;
	int mcs_per_node;
	int valid_cs_bits;
	bool mc_idx_valid;
};

static int read_ecc(struct mem_ctl_info *mci)
{
	struct loongson_edac_pvt *pvt = mci->pvt_info;
	u64 ecc;
	int cs;

	ecc = readq(pvt->ecc_base + ECC_CS_COUNT_REG);
	/* Discard the read value if any invalid bit is set to 1 */
	if (pvt->valid_cs_bits < 64 && (ecc >> pvt->valid_cs_bits)) {
		edac_mc_printk(mci, KERN_DEBUG, "ECC read invalid, skip: 0x%llx\n", ecc);
		return pvt->last_ce_count;
	}

	/* cs0 -- cs7 */
	cs = ecc & 0xff;
	cs += (ecc >> 8) & 0xff;
	cs += (ecc >> 16) & 0xff;
	cs += (ecc >> 24) & 0xff;
	cs += (ecc >> 32) & 0xff;
	cs += (ecc >> 40) & 0xff;
	cs += (ecc >> 48) & 0xff;
	cs += (ecc >> 56) & 0xff;

	return cs;
}

static void edac_check(struct mem_ctl_info *mci)
{
	struct loongson_edac_pvt *pvt = mci->pvt_info;
	char other_detail[64] = {0};
	int new, add, node, mc;

	new = read_ecc(mci);
	add = new - pvt->last_ce_count;
	pvt->last_ce_count = new;
	if (add <= 0)
		return;

	if (pvt->mc_idx_valid) {
		node = mci->mc_idx / pvt->mcs_per_node;
		mc = mci->mc_idx % pvt->mcs_per_node;
		snprintf(other_detail, sizeof(other_detail), "node:%d mc:%d", node, mc);
	}

	edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci, add,
			     0, 0, 0, 0, 0, -1, "error", other_detail);
}

static u32 loongson_get_dimm_size_acpi(struct device *dev)
{
	acpi_handle handle = ACPI_HANDLE(dev);
	union acpi_object *obj;
	u32 size_mb = 1;
	guid_t guid;

	if (!handle)
		return size_mb;

	if (guid_parse(LOONGSON_EDAC_DSM_UUID, &guid))
		return size_mb;

	obj = acpi_evaluate_dsm(handle, &guid, 0,
				LOONGSON_EDAC_DSM_FUNC_GET_DIMM_SIZE, NULL);
	if (!obj)
		return size_mb;

	if (obj->type == ACPI_TYPE_INTEGER)
		size_mb = (u32)obj->integer.value;

	ACPI_FREE(obj);
	return size_mb;
}

static void dimm_config_init(struct mem_ctl_info *mci)
{
	struct dimm_info *dimm;
	struct device *dev = mci->pdev;
	u32 size, npages;

	size = loongson_get_dimm_size_acpi(dev);

	npages = MiB_TO_PAGES(size);

	dimm = edac_get_dimm(mci, 0, 0, 0);
	dimm->nr_pages = npages;
	snprintf(dimm->label, sizeof(dimm->label),
		 "MC#%uChannel#%u_DIMM#%u", mci->mc_idx, 0, 0);
	dimm->grain = 8;
}

static void pvt_init(struct mem_ctl_info *mci, void __iomem *vbase,
		     bool mc_idx_valid, int mcs_per_node, int valid_cs_bits)
{
	struct loongson_edac_pvt *pvt = mci->pvt_info;

	pvt->ecc_base = vbase;
	pvt->mc_idx_valid = mc_idx_valid;
	pvt->mcs_per_node = mcs_per_node;
	pvt->valid_cs_bits = valid_cs_bits;
	pvt->last_ce_count = read_ecc(mci);
}

static int edac_probe(struct platform_device *pdev)
{
	struct edac_mc_layer layers[2];
	struct mem_ctl_info *mci;
	struct device *dev = &pdev->dev;
	void __iomem *vbase;
	u32 mcs_per_node, valid_cs_bits, mc_idx_u32;
	int ret;
	bool mc_idx_valid = true;

	vbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vbase))
		return PTR_ERR(vbase);

	layers[0].type = EDAC_MC_LAYER_CHANNEL;
	layers[0].size = 1;
	layers[0].is_virt_csrow = false;
	layers[1].type = EDAC_MC_LAYER_SLOT;
	layers[1].size = 1;
	layers[1].is_virt_csrow = true;
	mci = edac_mc_alloc(0, ARRAY_SIZE(layers), layers,
			    sizeof(struct loongson_edac_pvt));
	if (mci == NULL)
		return -ENOMEM;

	mci->mtype_cap = MEM_FLAG_RDDR4;
	mci->edac_ctl_cap = EDAC_FLAG_NONE;
	mci->edac_cap = EDAC_FLAG_NONE;
	mci->mod_name = "loongson_edac.c";
	mci->ctl_name = "loongson_edac_ctl";
	mci->dev_name = "loongson_edac_dev";
	mci->ctl_page_to_phys = NULL;
	mci->pdev = &pdev->dev;
	mci->error_desc.grain = 8;
	mci->edac_check = edac_check;

	if (device_property_read_u32(dev, "mc-idx", &mc_idx_u32)) {
		mci->mc_idx = edac_device_alloc_index();
		mc_idx_valid = false;
	} else {
		mci->mc_idx = mc_idx_u32;
	}

	if (device_property_read_u32(dev, "mc-per-node", &mcs_per_node) || mcs_per_node == 0)
		mcs_per_node = 4;

	if (device_property_read_u32(dev, "valid-cs-bits", &valid_cs_bits))
		valid_cs_bits = 32;

	pvt_init(mci, vbase, mc_idx_valid, mcs_per_node, valid_cs_bits);
	dimm_config_init(mci);

	ret = edac_mc_add_mc(mci);
	if (ret) {
		edac_dbg(0, "MC: failed edac_mc_add_mc()\n");
		edac_mc_free(mci);
		return ret;
	}
	edac_op_state = EDAC_OPSTATE_POLL;

	return 0;
}

static void edac_remove(struct platform_device *pdev)
{
	struct mem_ctl_info *mci = edac_mc_del_mc(&pdev->dev);

	if (mci)
		edac_mc_free(mci);
}

static const struct acpi_device_id loongson_edac_acpi_match[] = {
	{"LOON0010", 0},
	{}
};
MODULE_DEVICE_TABLE(acpi, loongson_edac_acpi_match);

static struct platform_driver loongson_edac_driver = {
	.probe		= edac_probe,
	.remove		= edac_remove,
	.driver		= {
		.name	= "loongson-mc-edac",
		.acpi_match_table = loongson_edac_acpi_match,
	},
};
module_platform_driver(loongson_edac_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zhao Qunqin <zhaoqunqin@loongson.cn>");
MODULE_DESCRIPTION("EDAC driver for loongson memory controller");
