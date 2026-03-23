// SPDX-License-Identifier: GPL-2.0-only
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/kernel.h>

#include "iommu.h"

static const struct debugfs_reg32 riscv_iommu_regs[] = {
	/* --- Global Configuration --- */
	{ .name = "capabilities",       .offset = RISCV_IOMMU_REG_CAPABILITIES  },
	{ .name = "fctl",               .offset = RISCV_IOMMU_REG_FCTL          },
	{ .name = "ddtp",               .offset = RISCV_IOMMU_REG_DDTP          },
	/* --- Command Queue --- */
	{ .name = "cqb",                .offset = RISCV_IOMMU_REG_CQB           },
	{ .name = "cqh",                .offset = RISCV_IOMMU_REG_CQH           },
	{ .name = "cqt",                .offset = RISCV_IOMMU_REG_CQT           },
	{ .name = "cqcsr",              .offset = RISCV_IOMMU_REG_CQCSR         },
	/* --- Fault Queue --- */
	{ .name = "fqb",                .offset = RISCV_IOMMU_REG_FQB           },
	{ .name = "fqh",                .offset = RISCV_IOMMU_REG_FQH           },
	{ .name = "fqt",                .offset = RISCV_IOMMU_REG_FQT           },
	{ .name = "fqcsr",              .offset = RISCV_IOMMU_REG_FQCSR         },
	/* --- Interrupts --- */
	{ .name = "ipsr",               .offset = RISCV_IOMMU_REG_IPSR          },
	{ .name = "icvec",              .offset = RISCV_IOMMU_REG_ICVEC         },
};

void riscv_iommu_debugfs_init(struct riscv_iommu_device *iommu)
{
	struct debugfs_regset32 *regset;

	if (!iommu_debugfs_dir)
		return;

	iommu->debugfs_dir = debugfs_create_dir(dev_name(iommu->dev), iommu_debugfs_dir);
	if (IS_ERR_OR_NULL(iommu->debugfs_dir))
		return;

	regset = devm_kzalloc(iommu->dev, sizeof(*regset), GFP_KERNEL);
	if (!regset)
		return;

	regset->regs = riscv_iommu_regs;
	regset->nregs = ARRAY_SIZE(riscv_iommu_regs);
	regset->base = iommu->reg;

	debugfs_create_regset32("registers", 0444, iommu->debugfs_dir, regset);
}

void riscv_iommu_debugfs_remove(struct riscv_iommu_device *iommu)
{
	debugfs_remove_recursive(iommu->debugfs_dir);
}
