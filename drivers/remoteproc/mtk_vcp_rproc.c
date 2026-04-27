// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 MediaTek Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/remoteproc.h>

#include "mtk_vcp_common.h"
#include "mtk_vcp_rproc.h"
#include "remoteproc_internal.h"

/**
 * vcp_get() - get a reference to VCP.
 *
 * @pdev: the platform device of the module requesting VCP platform
 *        device for using VCP API.
 *
 * Return: Return NULL if failed.  otherwise reference to VCP.
 **/
struct mtk_vcp_device *vcp_get(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *vcp_node;
	struct platform_device *vcp_pdev;

	vcp_node = of_parse_phandle(dev->of_node, "mediatek,vcp", 0);
	if (!vcp_node) {
		dev_err(dev, "can't get VCP node\n");
		return NULL;
	}

	vcp_pdev = of_find_device_by_node(vcp_node);
	of_node_put(vcp_node);

	if (!vcp_pdev)
		return NULL;

	return platform_get_drvdata(vcp_pdev);
}
EXPORT_SYMBOL_GPL(vcp_get);

/**
 * vcp_put() - release the reference to VCP.
 *
 * @vcp: the VCP device obtained from vcp_get().
 */
void vcp_put(struct mtk_vcp_device *vcp)
{
	put_device(vcp->dev);
}
EXPORT_SYMBOL_GPL(vcp_put);

/**
 * vcp_get_ipidev() - get a vcp ipi device struct to reference vcp ipi.
 *
 * @vcp: mtk_vcp_device structure from vcp_get().
 *
 * Return: Pointer to mtk_ipi_device structure.
 */
struct mtk_ipi_device *vcp_get_ipidev(struct mtk_vcp_device *vcp)
{
	return vcp->ipi_dev;
}
EXPORT_SYMBOL_GPL(vcp_get_ipidev);

static int mtk_vcp_start(struct rproc *rproc)
{
	struct mtk_vcp_device *vcp = rproc->priv;
	struct arm_smccc_res res;

	/* core 0 */
	arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
		      MTK_TINYSYS_VCP_KERNEL_OP_RESET_SET,
		      1, 0, 0, 0, 0, 0, &res);
	if (res.a0 != 1) {
		dev_err(vcp->dev, "VCP reset set SMC failed: %ld\n", res.a0);
		ret = -EIO;
		goto reset_failed;
	}

	/* core 1 */
	arm_smccc_smc(MTK_SIP_TINYSYS_VCP_CONTROL,
		      MTK_TINYSYS_MMUP_KERNEL_OP_RESET_SET,
		      1, 0, 0, 0, 0, 0, &res);
	if (res.a0 != 1) {
		dev_err(vcp->dev, "MMUP reset set SMC failed: %ld\n", res.a0);
		ret = -EIO;
		goto reset_failed;
	}

	ret = reset_vcp(vcp);
	if (ret) {
		dev_err(vcp->dev, "%s, VCP bootup failed\n", __func__);
		goto reset_failed;
	}

	dev_info(vcp->dev, "VCP bootup successfully\n");

	return 0;

reset_failed:

	return ret;
}

static int mtk_vcp_stop(struct rproc *rproc)
{
	return 0;
}

static const struct rproc_ops mtk_vcp_ops = {
	.load		= mtk_vcp_load,
	.start		= mtk_vcp_start,
	.stop		= mtk_vcp_stop,
};

static int vcp_ipi_mbox_init(struct mtk_vcp_device *vcp)
{
	struct mtk_vcp_ipc *vcp_ipc;
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_register_data(vcp->dev, "mtk-vcp-ipc",
					     PLATFORM_DEVID_NONE,
					     vcp->platdata->ipc_data,
					     sizeof(struct mtk_mbox_table));
	if (IS_ERR(pdev))
		return dev_err_probe(vcp->dev, PTR_ERR(pdev), "ipc_data register failed\n");

	ret = read_poll_timeout_atomic(dev_get_drvdata,
				       vcp_ipc, vcp_ipc,
				       USEC_PER_MSEC,
				       VCP_IPI_DEV_READY_TIMEOUT * USEC_PER_MSEC,
				       false, &pdev->dev);
	if (ret)
		return dev_err_probe(vcp->dev, -EPROBE_DEFER, "get vcp_ipc drvdata failed\n");

	ret = mtk_vcp_ipc_device_register(vcp->ipi_dev, VCP_IPI_COUNT, vcp_ipc);
	if (ret)
		dev_err_probe(vcp->dev, ret, "ipi_dev register failed, ret %d\n", ret);

	return ret;
}

static int vcp_multi_core_init(struct platform_device *pdev,
			       struct mtk_vcp_of_cluster *vcp_cluster,
			       enum vcp_core_id core_id)
{
	u32 num_harts;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "mediatek,vcp-core-harts",
				   &num_harts);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to get harts property\n");

	vcp_cluster->hart_count[core_id] = num_harts;

	ret = of_property_read_u32(pdev->dev.of_node, "mediatek,vcp-core-sram-offset",
				   &vcp_cluster->sram_offset[core_id]);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to get sram-offset property\n");

	return 0;
}

static struct mtk_vcp_device *vcp_rproc_init(struct platform_device *pdev,
					     struct mtk_vcp_of_cluster *vcp_cluster)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev_of_node(dev);
	struct device_node *child;
	struct platform_device *cpdev;
	struct mtk_vcp_device *vcp;
	struct rproc *rproc;
	const struct mtk_vcp_of_data *vcp_of_data;
	u32 core_id;
	int ret;

	vcp_of_data = of_device_get_match_data(dev);
	rproc = devm_rproc_alloc(dev, np->name, &mtk_vcp_ops,
				 vcp_of_data->platdata.fw_name,
				 sizeof(struct mtk_vcp_device));
	if (!rproc)
		return ERR_PTR(dev_err_probe(dev, -ENOMEM, "allocate remoteproc failed\n"));

	vcp  = rproc->priv;
	vcp->rproc = rproc;
	vcp->pdev = pdev;
	vcp->dev = dev;
	vcp->ops = &vcp_of_data->ops;
	vcp->platdata = &vcp_of_data->platdata;
	vcp->ipi_ops = vcp_of_data->platdata.ipi_ops;
	vcp->vcp_cluster = vcp_cluster;
	vcp->ipi_dev = &vcp_cluster->vcp_ipidev;

	rproc->auto_boot = vcp_of_data->platdata.auto_boot;
	rproc->sysfs_read_only = vcp_of_data->platdata.sysfs_read_only;
	platform_set_drvdata(pdev, vcp);

	ret = vcp_reserve_memory_init(vcp);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "reserve memory failed\n"));

	core_id = 0;
	for_each_available_child_of_node(np, child) {
		if (of_device_is_compatible(child, "mediatek,vcp-core")) {
			cpdev = of_find_device_by_node(child);
			if (!cpdev) {
				of_node_put(child);
				return ERR_PTR(dev_err_probe(dev, -ENODEV, "Not child node\n"));
			}
			ret = vcp_multi_core_init(cpdev, vcp_cluster, core_id);
			if (ret) {
				of_node_put(child);
				return ERR_PTR(dev_err_probe(dev, ret,
							     "vcp_multi_core_init failed\n"));
			}
			core_id++;
		}
	}
	vcp->vcp_cluster->core_nums = core_id;

	ret = vcp_wdt_irq_init(vcp);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "vcp_wdt_irq_init failed\n"));

	ret = vcp_ipi_mbox_init(vcp);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "vcp_ipi_mbox_init failed\n"));

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		return ERR_PTR(dev_err_probe(dev, ret, "Failed to runtime resume\n"));
	}

	return vcp;
}

static int vcp_cluster_init(struct platform_device *pdev,
			    struct mtk_vcp_of_cluster *vcp_cluster)
{
	struct mtk_vcp_device *vcp;
	int ret;

	vcp = vcp_rproc_init(pdev, vcp_cluster);
	if (IS_ERR(vcp))
		return dev_err_probe(vcp->dev, PTR_ERR(vcp), "vcp_rproc_init failed\n");

	ret = rproc_add(vcp->rproc);
	if (ret)
		return dev_err_probe(vcp->dev, ret, "Failed to add rproc\n");

	return 0;
}

static int vcp_device_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct mtk_vcp_of_cluster *vcp_cluster;
	int ret;

	pm_runtime_enable(dev);

	vcp_cluster = devm_kzalloc(dev, sizeof(*vcp_cluster), GFP_KERNEL);
	if (!vcp_cluster)
		return dev_err_probe(dev, -ENOMEM, "allocate resource failed\n");

	vcp_cluster->cfg = devm_platform_ioremap_resource_byname(pdev, "cfg");
	if (IS_ERR(vcp_cluster->cfg))
		return dev_err_probe(dev, PTR_ERR(vcp_cluster->cfg),
				     "Failed to parse and map cfg memory\n");

	vcp_cluster->cfg_sec = devm_platform_ioremap_resource_byname(pdev, "cfg_sec");
	if (IS_ERR(vcp_cluster->cfg_sec))
		return dev_err_probe(dev, PTR_ERR(vcp_cluster->cfg_sec),
				     "Failed to parse and map cfg_sec memory\n");

	vcp_cluster->cfg_core = devm_platform_ioremap_resource_byname(pdev, "cfg_core");
	if (IS_ERR(vcp_cluster->cfg_core))
		return dev_err_probe(dev, PTR_ERR(vcp_cluster->cfg_core),
				     "Failed to parse and map cfg_core memory\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	vcp_cluster->sram_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(vcp_cluster->sram_base))
		return dev_err_probe(dev, PTR_ERR(vcp_cluster->sram_base),
				     "Failed to parse and map sram memory\n");
	vcp_cluster->sram_size = (u32)resource_size(res);

	ret = devm_of_platform_populate(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to populate platform devices\n");

	ret = vcp_cluster_init(pdev, vcp_cluster);
	if (ret)
		return ret;

	return 0;
}

static void vcp_device_remove(struct platform_device *pdev)
{
	struct mtk_vcp_device *vcp = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);

	rproc_del(vcp->rproc);
}

static void vcp_device_shutdown(struct platform_device *pdev)
{
	struct mtk_vcp_device *vcp = platform_get_drvdata(pdev);
	int ret;

	writel(GIPC_VCP_HART0_SHUT, vcp->vcp_cluster->cfg_core + R_GIPC_IN_SET);
	ret = wait_core_hart_shutdown(vcp, VCP_ID);
	if (ret)
		dev_err(&pdev->dev, "wait VCP_ID core hart shutdown timeout\n");

	if (vcp->vcp_cluster->core_nums > MMUP_ID) {
		writel(GIPC_MMUP_SHUT, vcp->vcp_cluster->cfg_core + R_GIPC_IN_SET);
		ret = wait_core_hart_shutdown(vcp, MMUP_ID);
		if (ret)
			dev_err(&pdev->dev, "wait MMUP_ID core hart shutdown timeout\n");
	}
}

static struct mtk_vcp_feature_table mt8196_feature_tb[NUM_FEATURE_ID] = {
	{ .feature_id = RTOS_FEATURE_ID,        .core_id = VCP_CORE_TOTAL },
	{ .feature_id = VDEC_FEATURE_ID,        .core_id = VCP_ID },
	{ .feature_id = VENC_FEATURE_ID,        .core_id = VCP_ID },
	{ .feature_id = MMDVFS_MMUP_FEATURE_ID, .core_id = MMUP_ID },
	{ .feature_id = MMDVFS_VCP_FEATURE_ID,  .core_id = VCP_ID },
	{ .feature_id = MMDEBUG_FEATURE_ID,     .core_id = MMUP_ID },
	{ .feature_id = VMM_FEATURE_ID,         .core_id = MMUP_ID },
	{ .feature_id = VDISP_FEATURE_ID,       .core_id = MMUP_ID },
	{ .feature_id = MMQOS_FEATURE_ID,       .core_id = VCP_ID },
};

static struct mtk_vcp_reserved_mem_table mt8196_memory_tb[NUMS_MEM_ID] = {
	{ .memory_id = VCP_RTOS_MEM_ID,     .size = 0x1a00000 },
	{ .memory_id = VDEC_MEM_ID,         .size = 0x30000 },
	{ .memory_id = VENC_MEM_ID,         .size = 0x12000 },
	{ .memory_id = MMDVFS_VCP_MEM_ID,   .size = 0x1000 },
	{ .memory_id = MMDVFS_MMUP_MEM_ID,  .size = 0x1000 },
	{ .memory_id = MMQOS_MEM_ID,        .size = 0x1000 },
};

static struct mtk_mbox_table mt8196_ipc_tb = {
	.send_table = {
		{ .msg_size = 18, .ipi_id =  0, .mbox_id = 0 },

		{ .msg_size =  8, .ipi_id = 15, .mbox_id = 1 },
		{ .msg_size = 18, .ipi_id = 16, .mbox_id = 1 },
		{ .msg_size =  2, .ipi_id =  9, .mbox_id = 1 },

		{ .msg_size = 18, .ipi_id = 11, .mbox_id = 2 },
		{ .msg_size =  2, .ipi_id =  2, .mbox_id = 2 },
		{ .msg_size =  3, .ipi_id =  3, .mbox_id = 2 },
		{ .msg_size =  2, .ipi_id = 32, .mbox_id = 2 },

		{ .msg_size =  2, .ipi_id = 33, .mbox_id = 3 },
		{ .msg_size =  2, .ipi_id = 13, .mbox_id = 3 },
		{ .msg_size =  2, .ipi_id = 35, .mbox_id = 3 },

		{ .msg_size =  2, .ipi_id = 20, .mbox_id = 4 },
		{ .msg_size =  3, .ipi_id = 21, .mbox_id = 4 },
		{ .msg_size =  2, .ipi_id = 23, .mbox_id = 4 }
	},
	.recv_table = {
		{ .recv_opt = 0, .msg_size = 18, .ipi_id =  1, .mbox_id = 0 },

		{ .recv_opt = 1, .msg_size =  8, .ipi_id = 15, .mbox_id = 1 },
		{ .recv_opt = 0, .msg_size = 18, .ipi_id = 17, .mbox_id = 1 },
		{ .recv_opt = 0, .msg_size =  2, .ipi_id = 10, .mbox_id = 1 },

		{ .recv_opt = 0, .msg_size = 18, .ipi_id = 12, .mbox_id = 2 },
		{ .recv_opt = 0, .msg_size =  1, .ipi_id =  5, .mbox_id = 2 },
		{ .recv_opt = 1, .msg_size =  1, .ipi_id =  2, .mbox_id = 2 },

		{ .recv_opt = 0, .msg_size =  2, .ipi_id = 34, .mbox_id = 3 },
		{ .recv_opt = 0, .msg_size =  2, .ipi_id = 14, .mbox_id = 3 },

		{ .recv_opt = 0, .msg_size =  1, .ipi_id = 26, .mbox_id = 4 },
		{ .recv_opt = 1, .msg_size =  1, .ipi_id = 20, .mbox_id = 4 }
	},
	.recv_count = 11,
	.send_count = 14,
};

static struct mtk_vcp_ipi_ops mt8196_vcp_ipi_ops = {
	.ipi_send = mtk_vcp_ipc_send,
	.ipi_send_compl = mtk_vcp_ipc_send_compl,
	.ipi_register = mtk_vcp_mbox_ipc_register,
	.ipi_unregister = mtk_vcp_mbox_ipc_unregister,
};

static const struct mtk_vcp_of_data mt8196_of_data = {
	.ops = {
		.get_mem_phys = vcp_get_reserve_mem_phys,
		.get_mem_iova = vcp_get_reserve_mem_iova,
		.get_mem_virt = vcp_get_reserve_mem_virt,
		.get_mem_size = vcp_get_reserve_mem_size,
		.get_vcp_sram_virt = vcp_get_internal_sram_virt,
	},
	.platdata = {
		.auto_boot = true,
		.sysfs_read_only = true,
		.rtos_static_iova = 0x180600000,
		.ipc_data = &mt8196_ipc_tb,
		.ipi_ops = &mt8196_vcp_ipi_ops,
		.feature_tb = mt8196_feature_tb,
		.memory_tb = mt8196_memory_tb,
		.fw_name = "mediatek/mt8196/vcp.img",
	},
};

static const struct of_device_id mtk_vcp_of_match[] = {
	{ .compatible = "mediatek,mt8196-vcp", .data = &mt8196_of_data},
	{}
};
MODULE_DEVICE_TABLE(of, mtk_vcp_of_match);

static struct platform_driver mtk_vcp_device = {
	.probe = vcp_device_probe,
	.remove = vcp_device_remove,
	.shutdown = vcp_device_shutdown,
	.driver = {
		.name = "mtk-vcp",
		.of_match_table = mtk_vcp_of_match,
	},
};

module_platform_driver(mtk_vcp_device);

MODULE_AUTHOR("Xiangzhi Tang <xiangzhi.tang@mediatek.com>");
MODULE_DESCRIPTION("MTK VCP Controller");
MODULE_LICENSE("GPL");
