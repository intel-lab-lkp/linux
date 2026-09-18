// SPDX-License-Identifier: GPL-2.0+
/*
 * rcar-fcp.c  --  R-Car Frame Compression Processor Driver
 *
 * Copyright (C) 2016 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <media/rcar-fcp.h>

#define RCAR_FCP_REG_VCR			0x0000
#define RCAR_FCP_REG_VCR_CATEGORY		(1 << 8)
#define RCAR_FCP_REG_VCR_REVISION_H3_ES1	(1 << 0)
#define RCAR_FCP_REG_VCR_REVISION_M3W		(2 << 0)
#define RCAR_FCP_REG_VCR_REVISION_V3M		(3 << 0)
#define RCAR_FCP_REG_VCR_REVISION_H3		(4 << 0)
#define RCAR_FCP_REG_VCR_REVISION_D3		(5 << 0)
#define RCAR_FCP_REG_VCR_REVISION_M3N		(6 << 0)
#define RCAR_FCP_REG_VCR_REVISION_V3H		(7 << 0)
#define RCAR_FCP_REG_VCR_REVISION_E3		(8 << 0)
#define RCAR_FCP_REG_VCR_REVISION_X5H_V1	(12 << 0)

#define RCAR_FCP_REG_CFG0			0x0004
#define RCAR_FCP_REG_CFG0_FCPVSEL		BIT(1)

#define RCAR_FCP_REG_RST			0x0010
#define RCAR_FCP_REG_RST_RIIFRST		BIT(22)
#define RCAR_FCP_REG_RST_RSIFRST		BIT(21)
#define RCAR_FCP_REG_RST_DCMPRST		BIT(20)
#define RCAR_FCP_REG_RST_MODRST			BIT(4)
#define RCAR_FCP_REG_RST_SOFTRST		BIT(0)

#define RCAR_FCP_REG_STA			0x0018
#define RCAR_FCP_REG_STA_ACT			BIT(0)

#define RCAR_FCP_REG_TL_CTRL			0x0070
#define RCAR_FCP_REG_TL_CTRL_TLEN		BIT(31)
#define RCAR_FCP_REG_TL_CTRL_VPOS_C(n)		((n) << 16)
#define RCAR_FCP_REG_TL_CTRL_VPOS_Y(n)		((n) << 0)

#define RCAR_FCP_REG_PICINFO1			0x00c4
#define RCAR_FCP_REG_PICINFO1_STRIDE_DIV16	((n) << 0)

#define RCAR_FCP_REG_BA_ANC_Y0			0x0100
#define RCAR_FCP_REG_BA_ANC_Y1			0x0104
#define RCAR_FCP_REG_BA_ANC_Y2			0x0108
#define RCAR_FCP_REG_BA_ANC_C			0x010c
#define RCAR_FCP_REG_BA_REF_Y0			0x0110
#define RCAR_FCP_REG_BA_REF_Y1			0x0114
#define RCAR_FCP_REG_BA_REF_Y2			0x0118
#define RCAR_FCP_REG_BA_REF_C			0x011c

enum rcar_fcp_type {
	RCAR_FCPF,
	RCAR_FCPV,
};

struct rcar_fcp_device {
	struct list_head list;
	struct device *dev;
	void __iomem *base;
	struct reset_control *rstc;
	enum rcar_fcp_type type;
};

static LIST_HEAD(fcp_devices);
static DEFINE_MUTEX(fcp_lock);

static inline u32 rcar_fcp_read(struct rcar_fcp_device *fcp, u32 reg)
{
	return ioread32(fcp->base + reg);
}

static inline void rcar_fcp_write(struct rcar_fcp_device *fcp, u32 reg, u32 val)
{
	iowrite32(val, fcp->base + reg);
}

/* -----------------------------------------------------------------------------
 * Public API
 */

/**
 * rcar_fcp_get - Find and acquire a reference to an FCP instance
 * @np: Device node of the FCP instance
 *
 * Search the list of registered FCP instances for the instance corresponding to
 * the given device node.
 *
 * Return a pointer to the FCP instance, or an ERR_PTR if the instance can't be
 * found.
 */
struct rcar_fcp_device *rcar_fcp_get(const struct device_node *np)
{
	struct rcar_fcp_device *fcp;

	mutex_lock(&fcp_lock);

	list_for_each_entry(fcp, &fcp_devices, list) {
		if (fcp->dev->of_node != np)
			continue;

		get_device(fcp->dev);
		goto done;
	}

	fcp = ERR_PTR(-EPROBE_DEFER);

done:
	mutex_unlock(&fcp_lock);
	return fcp;
}
EXPORT_SYMBOL_GPL(rcar_fcp_get);

/**
 * rcar_fcp_put - Release a reference to an FCP instance
 * @fcp: The FCP instance
 *
 * Release the FCP instance acquired by a call to rcar_fcp_get().
 */
void rcar_fcp_put(struct rcar_fcp_device *fcp)
{
	if (fcp)
		put_device(fcp->dev);
}
EXPORT_SYMBOL_GPL(rcar_fcp_put);

struct device *rcar_fcp_get_device(struct rcar_fcp_device *fcp)
{
	return fcp->dev;
}
EXPORT_SYMBOL_GPL(rcar_fcp_get_device);

/**
 * rcar_fcp_enable - Enable an FCP
 * @fcp: The FCP instance
 *
 * Before any memory access through an FCP is performed by a module, the FCP
 * must be enabled by a call to this function. The enable calls are reference
 * counted, each successful call must be followed by one rcar_fcp_disable()
 * call when no more memory transfer can occur through the FCP.
 *
 * Return 0 on success or a negative error code if an error occurs. The enable
 * reference count isn't increased when this function returns an error.
 */
int rcar_fcp_enable(struct rcar_fcp_device *fcp)
{
	if (!fcp)
		return 0;

	return pm_runtime_resume_and_get(fcp->dev);
}
EXPORT_SYMBOL_GPL(rcar_fcp_enable);

/**
 * rcar_fcp_disable - Disable an FCP
 * @fcp: The FCP instance
 *
 * This function is the counterpart of rcar_fcp_enable(). As enable calls are
 * reference counted a disable call may not disable the FCP synchronously.
 */
void rcar_fcp_disable(struct rcar_fcp_device *fcp)
{
	if (fcp)
		pm_runtime_put(fcp->dev);
}
EXPORT_SYMBOL_GPL(rcar_fcp_disable);

int rcar_fcp_soft_reset(struct rcar_fcp_device *fcp)
{
	u32 value;
	int ret;

	if (!fcp)
		return 0;

	rcar_fcp_write(fcp, RCAR_FCP_REG_RST, RCAR_FCP_REG_RST_SOFTRST);
	ret = readl_poll_timeout(fcp->base + RCAR_FCP_REG_STA,
				 value, !(value & RCAR_FCP_REG_STA_ACT),
				 1, 100);
	if (ret)
		dev_err(fcp->dev, "Failed to soft-reset\n");

	return ret;
}
EXPORT_SYMBOL_GPL(rcar_fcp_soft_reset);

/* -----------------------------------------------------------------------------
 * Platform Driver
 */

static int rcar_fcp_setup(struct rcar_fcp_device *fcp)
{
	static const char * const models[] = {
		[RCAR_FCPF] = "FCPF",
		[RCAR_FCPV] = "FCPV",
	};
	static struct {
		u32 version;
		const char *name;
	} versions[] = {
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_H3_ES1, "H3 ES1.x" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_M3W, "M3W" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_V3M, "V3M" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_H3, "H3" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_D3, "D3" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_M3N, "M3N" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_V3H, "V3H" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_E3, "E3" },
		{ RCAR_FCP_REG_VCR_CATEGORY | RCAR_FCP_REG_VCR_REVISION_X5H_V1, "X5H v1.0" },
	};

	unsigned int i;
	u32 version;

	/* Check the device version register. */
	version = rcar_fcp_read(fcp, RCAR_FCP_REG_VCR);

	for (i = 0; i < ARRAY_SIZE(versions); ++i) {
		if (versions[i].version == version)
			break;
	}

	if (i >= ARRAY_SIZE(versions)) {
		dev_err(fcp->dev, "Invalid FCP version 0x%08x\n", version);
		return -ENODEV;
	}

	dev_dbg(fcp->dev, "%s %s device found\n", models[fcp->type],
		versions[i].name);

	return 0;
}

static const struct of_device_id rcar_fcp_of_match[] = {
	{ .compatible = "renesas,fcpf", .data = (void *)RCAR_FCPF },
	{ .compatible = "renesas,fcpv", .data = (void *)RCAR_FCPV },
	{ },
};
MODULE_DEVICE_TABLE(of, rcar_fcp_of_match);

static int rcar_fcp_probe(struct platform_device *pdev)
{
	struct rcar_fcp_device *fcp;
	int ret;

	fcp = devm_kzalloc(&pdev->dev, sizeof(*fcp), GFP_KERNEL);
	if (fcp == NULL)
		return -ENOMEM;

	fcp->dev = &pdev->dev;
	fcp->type = (enum rcar_fcp_type)device_get_match_data(&pdev->dev);

	platform_set_drvdata(pdev, fcp);

	fcp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fcp->base))
		return PTR_ERR(fcp->base);

	dma_set_max_seg_size(fcp->dev, UINT_MAX);

	fcp->rstc = devm_reset_control_get_optional(&pdev->dev, NULL);
	if (IS_ERR(fcp->rstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(fcp->rstc),
				     "failed to get reset control\n");

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0)
		goto error_pm_disable;

	ret = rcar_fcp_setup(fcp);
	if (ret < 0)
		goto error_pm_put;

	pm_runtime_put(&pdev->dev);

	mutex_lock(&fcp_lock);
	list_add_tail(&fcp->list, &fcp_devices);
	mutex_unlock(&fcp_lock);

	return 0;

error_pm_put:
	pm_runtime_put(&pdev->dev);
error_pm_disable:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static void rcar_fcp_remove(struct platform_device *pdev)
{
	struct rcar_fcp_device *fcp = platform_get_drvdata(pdev);

	mutex_lock(&fcp_lock);
	list_del(&fcp->list);
	mutex_unlock(&fcp_lock);

	pm_runtime_disable(&pdev->dev);
}

static int fcp_pm_runtime_suspend(struct device *dev)
{
	struct rcar_fcp_device *fcp = dev_get_drvdata(dev);

	return reset_control_assert(fcp->rstc);
}

static int fcp_pm_runtime_resume(struct device *dev)
{
	struct rcar_fcp_device *fcp = dev_get_drvdata(dev);

	return reset_control_deassert(fcp->rstc);
}

static const struct dev_pm_ops fcp_pm_ops = {
	RUNTIME_PM_OPS(fcp_pm_runtime_suspend, fcp_pm_runtime_resume, NULL)
};

static struct platform_driver rcar_fcp_platform_driver = {
	.probe		= rcar_fcp_probe,
	.remove		= rcar_fcp_remove,
	.driver		= {
		.name	= "rcar-fcp",
		.pm = pm_ptr(&fcp_pm_ops),
		.of_match_table = rcar_fcp_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(rcar_fcp_platform_driver);

MODULE_ALIAS("rcar-fcp");
MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Renesas FCP Driver");
MODULE_LICENSE("GPL");
