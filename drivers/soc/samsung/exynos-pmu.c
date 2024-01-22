// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2011-2014 Samsung Electronics Co., Ltd.
//		http://www.samsung.com/
//
// Exynos - CPU PMU(Power Management Unit) support

#include <linux/arm-smccc.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/mfd/core.h>
#include <linux/mfd/syscon.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/regmap.h>

#include <linux/soc/samsung/exynos-regs-pmu.h>
#include <linux/soc/samsung/exynos-pmu.h>

#include "exynos-pmu.h"

/**
 * DOC: Quirk flags for different Exynos PMU IP-cores
 *
 * This driver supports multiple Exynos based SoCs, each of which might have a
 * different set of registers and features supported.
 *
 * Quirk flags described below serve the purpose of telling the driver about
 * mentioned SoC traits, and can be specified in driver data for each particular
 * supported device.
 *
 * %QUIRK_HAS_ATOMIC_BITSETHW: PMU IP has special atomic bit set/clear HW
 * to protect against PMU registers being accessed from multiple bus masters.
 *
 * %QUIRK_PMU_ALIVE_WRITE_SEC: PMU registers are *not* write accesible from
 * normal world. This is found on some SoCs as a security hardening measure. PMU
 * registers on these SoCs can only be written via a SMC call and registers are
 * checked by EL3 firmware against an allowlist before the write can procede.
 * Note: This quirk should only be set for platforms whose el3 firmware
 * implements the TENSOR_SMC_PMU_SEC_REG interface below.
 */

#define QUIRK_HAS_ATOMIC_BITSETHW		BIT(0)
#define QUIRK_PMU_ALIVE_WRITE_SEC		BIT(1)

#define PMUALIVE_MASK GENMASK(14, 0)

struct exynos_pmu_context {
	struct device *dev;
	const struct exynos_pmu_data *pmu_data;
	struct regmap *pmureg;
	void __iomem *pmu_base_addr;
	phys_addr_t pmu_base_pa;
	/* protect PMU reg atomic update operations */
	spinlock_t update_lock;
};

static struct exynos_pmu_context *pmu_context;

/*
 * Some SoCs are configured so that PMU_ALIVE registers can only be written
 * from el3. As Linux needs to write some of these registers, the following
 * SMC register read/write/read,write,modify interface is used.
 *
 * Note: This SMC interface is known to be implemented on gs101 and derivative
 * SoCs.
 */
#define TENSOR_SMC_PMU_SEC_REG			(0x82000504)
#define TENSOR_PMUREG_READ			0
#define TENSOR_PMUREG_WRITE			1
#define TENSOR_PMUREG_RMW			2

int set_priv_reg(phys_addr_t reg, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(TENSOR_SMC_PMU_SEC_REG,
		      reg,
		      TENSOR_PMUREG_WRITE,
		      val, 0, 0, 0, 0, &res);

	if (res.a0)
		pr_warn("%s(): SMC failed: %lu\n", __func__, res.a0);

	return (int)res.a0;
}

int rmw_priv_reg(phys_addr_t reg, u32 mask, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(TENSOR_SMC_PMU_SEC_REG,
		      reg,
		      TENSOR_PMUREG_RMW,
		      mask, val, 0, 0, 0, &res);

	if (res.a0)
		pr_warn("%s(): SMC failed: %lu\n", __func__, res.a0);

	return (int)res.a0;
}

/*
 * For SoCs that have set/clear bit hardware (as indicated by
 * QUIRK_HAS_ATOMIC_BITSETHW) this function can be used when
 * the PMU register will be accessed by multiple masters.
 *
 * For example, to set bits 13:8 in PMU reg offset 0x3e80
 * exynos_pmu_set_bit_atomic(0x3e80, 0x3f00, 0x3f00);
 *
 * To clear bits 13:8 in PMU offset 0x3e80
 * exynos_pmu_set_bit_atomic(0x3e80, 0x0, 0x3f00);
 */
static inline void exynos_pmu_set_bit_atomic(unsigned int offset,
					     u32 val, u32 mask)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&pmu_context->update_lock, flags);
	for (i = 0; i < 32; i++) {
		if (mask & BIT(i)) {
			if (val & BIT(i)) {
				offset |= 0xc000;
				pmu_raw_writel(i, offset);
			} else {
				offset |= 0x8000;
				pmu_raw_writel(i, offset);
			}
		}
	}
	spin_unlock_irqrestore(&pmu_context->update_lock, flags);
}

int exynos_pmu_update_bits(unsigned int offset, unsigned int mask,
			   unsigned int val)
{
	if (pmu_context->pmu_data &&
	    pmu_context->pmu_data->quirks & QUIRK_PMU_ALIVE_WRITE_SEC)
		return rmw_priv_reg(pmu_context->pmu_base_pa + offset,
				    mask, val);

	return regmap_update_bits(pmu_context->pmureg, offset, mask, val);
}
EXPORT_SYMBOL(exynos_pmu_update_bits);

void pmu_raw_writel(u32 val, u32 offset)
{
	if (pmu_context->pmu_data &&
	    pmu_context->pmu_data->quirks & QUIRK_PMU_ALIVE_WRITE_SEC)
		return (void)set_priv_reg(pmu_context->pmu_base_pa + offset,
					  val);

	return writel_relaxed(val, pmu_context->pmu_base_addr + offset);
}

u32 pmu_raw_readl(u32 offset)
{
	return readl_relaxed(pmu_context->pmu_base_addr + offset);
}

int exynos_pmu_read(unsigned int offset, unsigned int *val)
{
	if (!pmu_context)
		return -ENODEV;

	/*
	 * For platforms that protect PMU registers they
	 * are still accessible to read from normal world
	 */
	return regmap_read(pmu_context->pmureg, offset, val);
}
EXPORT_SYMBOL(exynos_pmu_read);

int exynos_pmu_write(unsigned int offset, unsigned int val)
{
	if (!pmu_context)
		return -ENODEV;

	if (pmu_context->pmu_data &&
	    pmu_context->pmu_data->quirks & QUIRK_PMU_ALIVE_WRITE_SEC)
		return set_priv_reg(pmu_context->pmu_base_pa + offset, val);

	return regmap_write(pmu_context->pmureg, offset, val);
}
EXPORT_SYMBOL(exynos_pmu_write);

int exynos_pmu_update(unsigned int offset, unsigned int mask, unsigned int val)
{
	int ret = 0;

	if (!pmu_context)
		return -ENODEV;

	if (pmu_context->pmu_data &&
	    pmu_context->pmu_data->quirks & QUIRK_HAS_ATOMIC_BITSETHW) {
		/*
		 * Use atomic operations for PMU_ALIVE registers (offset 0~0x3FFF)
		 * as the target registers can be accessed by multiple masters.
		 */
		if (offset > PMUALIVE_MASK)
			return exynos_pmu_update_bits(offset, mask, val);

		exynos_pmu_set_bit_atomic(offset, val, mask);

	} else {
		return exynos_pmu_update_bits(offset, mask, val);
	}

	return ret;
}
EXPORT_SYMBOL(exynos_pmu_update);

void exynos_sys_powerdown_conf(enum sys_powerdown mode)
{
	unsigned int i;
	const struct exynos_pmu_data *pmu_data;

	if (!pmu_context || !pmu_context->pmu_data)
		return;

	pmu_data = pmu_context->pmu_data;

	if (pmu_data->powerdown_conf)
		pmu_data->powerdown_conf(mode);

	if (pmu_data->pmu_config) {
		for (i = 0; (pmu_data->pmu_config[i].offset != PMU_TABLE_END); i++)
			pmu_raw_writel(pmu_data->pmu_config[i].val[mode],
					pmu_data->pmu_config[i].offset);
	}

	if (pmu_data->powerdown_conf_extra)
		pmu_data->powerdown_conf_extra(mode);

	if (pmu_data->pmu_config_extra) {
		for (i = 0; pmu_data->pmu_config_extra[i].offset != PMU_TABLE_END; i++)
			pmu_raw_writel(pmu_data->pmu_config_extra[i].val[mode],
				       pmu_data->pmu_config_extra[i].offset);
	}
}

/*
 * Split the data between ARM architectures because it is relatively big
 * and useless on other arch.
 */
#ifdef CONFIG_EXYNOS_PMU_ARM_DRIVERS
#define exynos_pmu_data_arm_ptr(data)	(&data)
#else
#define exynos_pmu_data_arm_ptr(data)	NULL
#endif

static const struct exynos_pmu_data gs101_pmu_data = {
	.quirks = QUIRK_HAS_ATOMIC_BITSETHW | QUIRK_PMU_ALIVE_WRITE_SEC,
};

/*
 * PMU platform driver and devicetree bindings.
 */
static const struct of_device_id exynos_pmu_of_device_ids[] = {
	{
		.compatible = "google,gs101-pmu",
		.data = &gs101_pmu_data,
	}, {
		.compatible = "samsung,exynos3250-pmu",
		.data = exynos_pmu_data_arm_ptr(exynos3250_pmu_data),
	}, {
		.compatible = "samsung,exynos4210-pmu",
		.data = exynos_pmu_data_arm_ptr(exynos4210_pmu_data),
	}, {
		.compatible = "samsung,exynos4212-pmu",
		.data = exynos_pmu_data_arm_ptr(exynos4212_pmu_data),
	}, {
		.compatible = "samsung,exynos4412-pmu",
		.data = exynos_pmu_data_arm_ptr(exynos4412_pmu_data),
	}, {
		.compatible = "samsung,exynos5250-pmu",
		.data = exynos_pmu_data_arm_ptr(exynos5250_pmu_data),
	}, {
		.compatible = "samsung,exynos5410-pmu",
	}, {
		.compatible = "samsung,exynos5420-pmu",
		.data = exynos_pmu_data_arm_ptr(exynos5420_pmu_data),
	}, {
		.compatible = "samsung,exynos5433-pmu",
	}, {
		.compatible = "samsung,exynos7-pmu",
	}, {
		.compatible = "samsung,exynos850-pmu",
	},
	{ /*sentinel*/ },
};

static const struct mfd_cell exynos_pmu_devs[] = {
	{ .name = "exynos-clkout", },
};

struct regmap *exynos_get_pmu_regmap(void)
{
	struct device_node *np = of_find_matching_node(NULL,
						      exynos_pmu_of_device_ids);
	if (np)
		return syscon_node_to_regmap(np);
	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(exynos_get_pmu_regmap);

static int exynos_pmu_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	int ret;

	pmu_context = devm_kzalloc(&pdev->dev,
			sizeof(struct exynos_pmu_context),
			GFP_KERNEL);
	if (!pmu_context)
		return -ENOMEM;

	pmu_context->pmu_base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pmu_context->pmu_base_addr))
		return PTR_ERR(pmu_context->pmu_base_addr);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	pmu_context->pmu_base_pa = res->start;
	pmu_context->pmureg = exynos_get_pmu_regmap();
	if (IS_ERR(pmu_context->pmureg))
		return PTR_ERR(pmu_context->pmureg);

	spin_lock_init(&pmu_context->update_lock);
	pmu_context->dev = dev;
	pmu_context->pmu_data = of_device_get_match_data(dev);

	if (pmu_context->pmu_data && pmu_context->pmu_data->pmu_init)
		pmu_context->pmu_data->pmu_init();

	platform_set_drvdata(pdev, pmu_context);

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE, exynos_pmu_devs,
				   ARRAY_SIZE(exynos_pmu_devs), NULL, 0, NULL);
	if (ret)
		return ret;

	if (devm_of_platform_populate(dev))
		dev_err(dev, "Error populating children, reboot and poweroff might not work properly\n");

	dev_dbg(dev, "Exynos PMU Driver probe done\n");
	return 0;
}

static struct platform_driver exynos_pmu_driver = {
	.driver  = {
		.name   = "exynos-pmu",
		.of_match_table = exynos_pmu_of_device_ids,
	},
	.probe = exynos_pmu_probe,
};

static int __init exynos_pmu_init(void)
{
	return platform_driver_register(&exynos_pmu_driver);

}
postcore_initcall(exynos_pmu_init);
