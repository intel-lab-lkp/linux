// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/io.h>
#include <linux/soc/mediatek/mtk-dramc.h>

static struct platform_device *dramc_pdev;
static struct platform_driver dramc_drv;

static int fmeter_init(struct platform_device *pdev,
		       struct fmeter_dev_t *fmeter_dev_ptr, unsigned int fmeter_version)
{
	struct device_node *dramc_node = pdev->dev.of_node;
	int ret;

	ret = of_property_read_u32(dramc_node,
				   "crystal-freq", &(fmeter_dev_ptr->crystal_freq));
	ret |= of_property_read_u32(dramc_node,
				    "shu-of", &(fmeter_dev_ptr->shu_of));
	ret |= of_property_read_u32_array(dramc_node,
					  "shu-lv", (unsigned int *)&(fmeter_dev_ptr->shu_lv), 3);
	ret |= of_property_read_u32_array(dramc_node,
					  "pll-id", (unsigned int *)&(fmeter_dev_ptr->pll_id), 3);
	ret |= of_property_read_u32_array(dramc_node,
					  "sdmpcw", (unsigned int *)(fmeter_dev_ptr->sdmpcw), 6);
	ret |= of_property_read_u32_array(dramc_node,
					  "posdiv", (unsigned int *)(fmeter_dev_ptr->posdiv), 6);
	ret |= of_property_read_u32_array(dramc_node,
					  "fbksel", (unsigned int *)(fmeter_dev_ptr->fbksel), 6);
	ret |= of_property_read_u32_array(dramc_node,
					  "dqsopen", (unsigned int *)(fmeter_dev_ptr->dqsopen), 6);
	if (fmeter_version == 1) {
		fmeter_dev_ptr->version = 1;
		ret |= of_property_read_u32_array(dramc_node,
			"async-ca", (unsigned int *)(fmeter_dev_ptr->async_ca), 6);
		ret |= of_property_read_u32_array(dramc_node,
			"dq-ser-mode", (unsigned int *)(fmeter_dev_ptr->dq_ser_mode), 6);
	}
	return ret;
}

static ssize_t dram_data_rate_show(struct device_driver *driver, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "DRAM data rate = %d\n",
		mtk_dramc_get_data_rate());
}

static DRIVER_ATTR_RO(dram_data_rate);

static int dramc_probe(struct platform_device *pdev)
{
	struct device_node *dramc_node = pdev->dev.of_node;
	struct dramc_dev_t *dramc_dev_ptr;
	unsigned int fmeter_version;
	struct resource *res;
	unsigned int i, size;
	int ret;

	pr_info("%s: module probe.\n", __func__);
	dramc_pdev = pdev;
	dramc_dev_ptr = devm_kmalloc(&pdev->dev,
				     sizeof(struct dramc_dev_t), GFP_KERNEL);

	if (!dramc_dev_ptr)
		return -ENOMEM;

	ret = of_property_read_u32(dramc_node,
				   "support-ch-cnt", &dramc_dev_ptr->support_ch_cnt);
	if (ret) {
		pr_info("%s: get support_ch_cnt fail\n", __func__);
		return -EINVAL;
	}

	dramc_dev_ptr->sleep_base = of_iomap(dramc_node,
					     dramc_dev_ptr->support_ch_cnt * 4);
	if (IS_ERR(dramc_dev_ptr->sleep_base)) {
		pr_info("%s: unable to map sleep base\n", __func__);
		return -EINVAL;
	}

	size = sizeof(phys_addr_t) * dramc_dev_ptr->support_ch_cnt;
	dramc_dev_ptr->dramc_chn_base_ao = devm_kmalloc(&pdev->dev,
							size, GFP_KERNEL);
	if (!(dramc_dev_ptr->dramc_chn_base_ao))
		return -ENOMEM;
	dramc_dev_ptr->dramc_chn_base_nao = devm_kmalloc(&pdev->dev,
							 size, GFP_KERNEL);
	if (!(dramc_dev_ptr->dramc_chn_base_nao))
		return -ENOMEM;
	dramc_dev_ptr->ddrphy_chn_base_ao = devm_kmalloc(&pdev->dev,
							 size, GFP_KERNEL);
	if (!(dramc_dev_ptr->ddrphy_chn_base_ao))
		return -ENOMEM;
	dramc_dev_ptr->ddrphy_chn_base_nao = devm_kmalloc(&pdev->dev,
							  size, GFP_KERNEL);
	if (!(dramc_dev_ptr->ddrphy_chn_base_nao))
		return -ENOMEM;

	for (i = 0; i < dramc_dev_ptr->support_ch_cnt; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		dramc_dev_ptr->dramc_chn_base_ao[i] =
			devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(dramc_dev_ptr->dramc_chn_base_ao[i])) {
			pr_info("%s: unable to map ch%d DRAMC AO base\n",
				__func__, i);
			return -EINVAL;
		}

		res = platform_get_resource(pdev, IORESOURCE_MEM,
					    i + dramc_dev_ptr->support_ch_cnt);
		dramc_dev_ptr->dramc_chn_base_nao[i] =
			devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(dramc_dev_ptr->dramc_chn_base_nao[i])) {
			pr_info("%s: unable to map ch%d DRAMC NAO base\n",
				__func__, i);
			return -EINVAL;
		}

		res = platform_get_resource(pdev, IORESOURCE_MEM,
					    i + dramc_dev_ptr->support_ch_cnt * 2);
		dramc_dev_ptr->ddrphy_chn_base_ao[i] =
			devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(dramc_dev_ptr->ddrphy_chn_base_ao[i])) {
			pr_info("%s: unable to map ch%d DDRPHY AO base\n",
				__func__, i);
			return -EINVAL;
		}

		res = platform_get_resource(pdev, IORESOURCE_MEM,
					    i + dramc_dev_ptr->support_ch_cnt * 3);
		dramc_dev_ptr->ddrphy_chn_base_nao[i] =
			devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(dramc_dev_ptr->ddrphy_chn_base_nao[i])) {
			pr_info("%s: unable to map ch%d DDRPHY NAO base\n",
				__func__, i);
			return -EINVAL;
		}
	}

	ret = of_property_read_u32(dramc_node, "fmeter-version", &fmeter_version);
	if (ret) {
		pr_info("%s: get fmeter_version fail\n", __func__);
		return -EINVAL;
	}
	pr_info("%s: fmeter_version(%d)\n", __func__, fmeter_version);

	if (fmeter_version == 1) {
		dramc_dev_ptr->fmeter_dev_ptr = devm_kmalloc(&pdev->dev,
							     sizeof(struct fmeter_dev_t),
							     GFP_KERNEL);
		if (!(dramc_dev_ptr->fmeter_dev_ptr)) {
			pr_info("%s: memory  alloc fail\n", __func__);
			return -ENOMEM;
		}
		ret = fmeter_init(pdev, dramc_dev_ptr->fmeter_dev_ptr, fmeter_version);
		if (ret) {
			pr_info("%s: fmeter_init fail\n", __func__);
			return -EINVAL;
		}
	} else {
		dramc_dev_ptr->fmeter_dev_ptr = NULL;
	}
	ret = driver_create_file(pdev->dev.driver, &driver_attr_dram_data_rate);
	if (ret) {
		pr_info("%s: fail to create dram_data_rate sysfs\n", __func__);
		return ret;
	}

	platform_set_drvdata(pdev, dramc_dev_ptr);
	pr_info("%s: DRAM data rate = %d\n", __func__,
		mtk_dramc_get_data_rate());

	return ret;
}

static unsigned int fmeter_v1(struct dramc_dev_t *dramc_dev_ptr)
{
	struct fmeter_dev_t *fmeter_dev_ptr =
		(struct fmeter_dev_t *)dramc_dev_ptr->fmeter_dev_ptr;
	unsigned int shu_lv_val;
	unsigned int pll_id_val;
	unsigned int sdmpcw_val;
	unsigned int posdiv_val;
	unsigned int ckdiv4_val;
	unsigned int offset;
	unsigned int vco_freq;
	unsigned int fbksel;
	unsigned int dqsopen;
	unsigned int async_ca;
	unsigned int dq_ser_mode;

	shu_lv_val = (readl(dramc_dev_ptr->ddrphy_chn_base_ao[0] +
		fmeter_dev_ptr->shu_lv.offset) &
		fmeter_dev_ptr->shu_lv.mask) >>
		fmeter_dev_ptr->shu_lv.shift;

	pll_id_val = (readl(dramc_dev_ptr->ddrphy_chn_base_ao[0] +
		fmeter_dev_ptr->pll_id.offset) &
		fmeter_dev_ptr->pll_id.mask) >>
		fmeter_dev_ptr->pll_id.shift;

	offset = fmeter_dev_ptr->sdmpcw[pll_id_val].offset +
		fmeter_dev_ptr->shu_of * shu_lv_val;
	sdmpcw_val = (readl(dramc_dev_ptr->ddrphy_chn_base_nao[0] + offset) &
		fmeter_dev_ptr->sdmpcw[pll_id_val].mask) >>
		fmeter_dev_ptr->sdmpcw[pll_id_val].shift;

	offset = fmeter_dev_ptr->posdiv[pll_id_val].offset +
		fmeter_dev_ptr->shu_of * shu_lv_val;
	posdiv_val = (readl(dramc_dev_ptr->ddrphy_chn_base_nao[0] + offset) &
		fmeter_dev_ptr->posdiv[pll_id_val].mask) >>
		fmeter_dev_ptr->posdiv[pll_id_val].shift;

	offset = fmeter_dev_ptr->fbksel[pll_id_val].offset +
		fmeter_dev_ptr->shu_of * shu_lv_val;
	fbksel = (readl(dramc_dev_ptr->ddrphy_chn_base_nao[0] + offset) &
		fmeter_dev_ptr->fbksel[pll_id_val].mask) >>
		fmeter_dev_ptr->fbksel[pll_id_val].shift;

	offset = fmeter_dev_ptr->dqsopen[pll_id_val].offset +
		fmeter_dev_ptr->shu_of * shu_lv_val;
	dqsopen = (readl(dramc_dev_ptr->ddrphy_chn_base_nao[0] + offset) &
		fmeter_dev_ptr->dqsopen[pll_id_val].mask) >>
		fmeter_dev_ptr->dqsopen[pll_id_val].shift;

	offset = fmeter_dev_ptr->async_ca[pll_id_val].offset +
		fmeter_dev_ptr->shu_of * shu_lv_val;
	async_ca = (readl(dramc_dev_ptr->ddrphy_chn_base_nao[0] + offset) &
		fmeter_dev_ptr->async_ca[pll_id_val].mask) >>
		fmeter_dev_ptr->async_ca[pll_id_val].shift;

	offset = fmeter_dev_ptr->dq_ser_mode[pll_id_val].offset +
		fmeter_dev_ptr->shu_of * shu_lv_val;
	dq_ser_mode = (readl(dramc_dev_ptr->ddrphy_chn_base_nao[0] + offset) &
		fmeter_dev_ptr->dq_ser_mode[pll_id_val].mask) >>
		fmeter_dev_ptr->dq_ser_mode[pll_id_val].shift;
	ckdiv4_val = (dq_ser_mode == 1); // 1: DIV4, 2: DIV8, 3: DIV16

	posdiv_val &= ~(0x4);

	vco_freq = ((fmeter_dev_ptr->crystal_freq) *
		(sdmpcw_val >> 7)) >> posdiv_val >> 1 >> ckdiv4_val
		<< fbksel;

	if ((dqsopen == 1) && (async_ca == 1))
		vco_freq >>= 1;

	return vco_freq;
}

/*
 * mtk_dramc_get_data_rate - calculate DRAM data rate
 *
 * Returns DRAM data rate (MB/s)
 */
unsigned int mtk_dramc_get_data_rate(void)
{
	struct dramc_dev_t *dramc_dev_ptr;
	struct fmeter_dev_t *fmeter_dev_ptr;

	if (!dramc_pdev)
		return 0;

	dramc_dev_ptr =
		(struct dramc_dev_t *)platform_get_drvdata(dramc_pdev);

	fmeter_dev_ptr = (struct fmeter_dev_t *)dramc_dev_ptr->fmeter_dev_ptr;
	if (!fmeter_dev_ptr)
		return 0;

	if (fmeter_dev_ptr->version == 1)
		return fmeter_v1(dramc_dev_ptr);
	return 0;
}
EXPORT_SYMBOL(mtk_dramc_get_data_rate);

static int dramc_remove(struct platform_device *pdev)
{
	dramc_pdev = NULL;

	return 0;
}

static const struct of_device_id dramc_of_ids[] = {
	{.compatible = "mediatek,common-dramc",},
	{}
};

static struct platform_driver dramc_drv = {
	.probe = dramc_probe,
	.remove = dramc_remove,
	.driver = {
		.name = "dramc_drv",
		.owner = THIS_MODULE,
		.of_match_table = dramc_of_ids,
	},
};

static int __init dramc_drv_init(void)
{
	int ret;

	ret = platform_driver_register(&dramc_drv);
	if (ret) {
		pr_info("%s: init fail, ret 0x%x\n", __func__, ret);
		return ret;
	}

	return ret;
}

module_init(dramc_drv_init);

MODULE_AUTHOR("Mediatek Corporation");
MODULE_DESCRIPTION("MediaTek DRAMC Driver");
MODULE_LICENSE("GPL");
