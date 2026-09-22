// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner XuanTie E906/E907 RISC-V Remote Processor Driver
 *
 * Copyright (C) 2024-2026 Allwinner Technology Co., Ltd.
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 *
 * CCU-integrated remoteproc driver for XuanTie RISC-V co-processors on
 * Allwinner SoCs (T527/A527/A523: E906/E907) supporting TCM, SRAM, and DRAM.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

#include "remoteproc_internal.h"

#define DRIVER_NAME "sunxi-rproc"

/* Remap Control Register (offset 0x364 in PRCM_R_CCU / MCU_CCU) */
#define SUNXI_REMAP_CTRL_OFFSET		0x0364
/* Bit 0: 0 = local RAM for MCU; 1 = share for system */
#define SUNXI_REMAP_MCU_RAM_BIT		BIT(0)
/* Bit 1: 0 = SRAMA3_2 not shared; 1 = share for MCU_SYS */
#define SUNXI_REMAP_SRAMA3_2_BIT	BIT(1)

/* XuanTie core-local view of dedicated SRAM Spaces (E906/E907) */
#define E907_SRAM_C_DA			0x00020000
#define E907_SRAM_SPACE0_DA		0x3ff80000
#define E907_SRAM_SPACE0_DA_ALT		0x3ffc0000
#define E907_SRAM_SPACE1_DA		0x40000000

/* XuanTie E906/E907 Control & Boot Address Registers (when CFG block is present) */
#define E906_CTRL_REG			0x0000
#define E906_STA_ADD_REG		0x0204

struct sunxi_rproc_cfg {
	const char *name;
};

static const struct sunxi_rproc_cfg sun55i_riscv_cfg = {
	.name = "XuanTie E907 RISC-V",
};

struct sunxi_rproc {
	struct rproc *rproc;
	struct device *dev;
	const struct sunxi_rproc_cfg *cfg;

	/* CCU Clocks & Resets */
	struct clk *clk_parent;
	struct clk *clk_bus;
	struct clk *clk_core;
	struct clk *clk_sram;
	struct clk *clk_msgbox;
	struct reset_control *rst_cfg;
	struct reset_control *rst_core;
	struct reset_control *rst_sram;
	struct reset_control *rst_msgbox;

	/* Hardware Memory Windows (Dedicated SRAM, Switchable SRAM, Remap) */
	void __iomem *cfg_va;
	phys_addr_t cfg_phys;

	void __iomem *remap_va;
	phys_addr_t remap_phys;

	void __iomem *r_sram_va;
	phys_addr_t r_sram_phys;
	size_t r_sram_size;

	void __iomem *r_sram1_va;
	phys_addr_t r_sram1_phys;
	size_t r_sram1_size;

	void *dram_va;
	phys_addr_t dram_phys;
	size_t dram_size;

	/* Trace / DDR Reserved Memory Window */
	void *trace_va;
	phys_addr_t trace_phys;
	size_t trace_size;

	/* Reserved Memory & Mailbox State */
	bool has_reserved_mem;
	struct mbox_client cl;
	struct mbox_chan *tx_chan;
	struct mbox_chan *rx_chan;
	struct work_struct vq_work;
};

static void sunxi_rproc_vq_work(struct work_struct *work)
{
	struct sunxi_rproc *priv = container_of(work, struct sunxi_rproc, vq_work);

	rproc_vq_interrupt(priv->rproc, 0);
	rproc_vq_interrupt(priv->rproc, 1);
}

static void sunxi_rproc_mb_rx_callback(struct mbox_client *cl, void *data)
{
	struct sunxi_rproc *priv = container_of(cl, struct sunxi_rproc, cl);

	schedule_work(&priv->vq_work);
}

static irqreturn_t sunxi_rproc_crash_handler(int irq, void *data)
{
	struct rproc *rproc = data;
	struct sunxi_rproc *priv = rproc->priv;

	dev_err(priv->dev, "Hardware crash event received from %s core!\n",
		priv->cfg ? priv->cfg->name : "remote");
	rproc_report_crash(rproc, RPROC_FATAL_ERROR);

	return IRQ_HANDLED;
}

static int sunxi_rproc_prepare(struct rproc *rproc)
{
	struct sunxi_rproc *priv = rproc->priv;
	int ret;

	/* 1. Deassert configuration & SRAM bus resets */
	if (priv->rst_cfg) {
		ret = reset_control_deassert(priv->rst_cfg);
		if (ret) {
			dev_err(priv->dev, "failed to deassert cfg reset: %d\n", ret);
			return ret;
		}
	}

	if (priv->rst_sram) {
		ret = reset_control_deassert(priv->rst_sram);
		if (ret) {
			dev_err(priv->dev, "failed to deassert sram reset: %d\n", ret);
			goto err_assert_cfg;
		}
	}

	if (priv->rst_msgbox) {
		ret = reset_control_deassert(priv->rst_msgbox);
		if (ret) {
			dev_err(priv->dev, "failed to deassert msgbox reset: %d\n", ret);
			goto err_assert_sram;
		}
	}

	/* 2. Enable parent clock (PLL source) */
	if (priv->clk_parent) {
		ret = clk_prepare_enable(priv->clk_parent);
		if (ret) {
			dev_err(priv->dev, "failed to enable parent clock: %d\n", ret);
			goto err_assert_msgbox;
		}
	}

	/* 3. Enable interconnect bus and SRAM clocks */
	if (priv->clk_bus) {
		ret = clk_prepare_enable(priv->clk_bus);
		if (ret) {
			dev_err(priv->dev, "failed to enable bus clock: %d\n", ret);
			goto err_disable_parent;
		}
	}

	if (priv->clk_sram) {
		ret = clk_prepare_enable(priv->clk_sram);
		if (ret) {
			dev_err(priv->dev, "failed to enable sram clock: %d\n", ret);
			goto err_disable_bus;
		}
	}

	if (priv->clk_msgbox) {
		ret = clk_prepare_enable(priv->clk_msgbox);
		if (ret) {
			dev_err(priv->dev, "failed to enable msgbox clock: %d\n", ret);
			goto err_disable_sram_clk;
		}
	}

	/* 4. Enable core clock */
	if (priv->clk_core) {
		ret = clk_prepare_enable(priv->clk_core);
		if (ret) {
			dev_err(priv->dev, "failed to enable core clock: %d\n", ret);
			goto err_disable_msgbox_clk;
		}
	}

	/*
	 * 4b. Enable SRAMA3_2 for MCU_SYS (RISC-V) via REMAP_CTRL_REG bit 1.
	 */
	if (priv->remap_va) {
		u32 remap_val = readl(priv->remap_va);

		remap_val |= SUNXI_REMAP_SRAMA3_2_BIT;
		writel(remap_val, priv->remap_va);
		dev_dbg(priv->dev, "REMAP_CTRL_REG set to 0x%08x (SRAMA3_2 enabled)\n",
			readl(priv->remap_va));
	}

	/*
	 * 5. Cleanly clear Dedicated Local SRAM and Switchable SRAM.
	 * Only clears regions that are actually mapped from Device Tree.
	 * Matches upstream patterns (e.g., imx_rproc / ti_k3_r5_remoteproc) to:
	 *  - Prevent ECC/parity noise on uninitialized memory banks.
	 *  - Ensure NOBITS / .bss sections start strictly at zero.
	 *  - Clear stale trace0 logs/telemetry from previous runs.
	 */
	if (priv->r_sram_va && priv->r_sram_size)
		memset_io(priv->r_sram_va, 0, priv->r_sram_size);

	if (priv->r_sram1_va && priv->r_sram1_size)
		memset_io(priv->r_sram1_va, 0, priv->r_sram1_size);

	return 0;

err_disable_msgbox_clk:
	if (priv->clk_msgbox)
		clk_disable_unprepare(priv->clk_msgbox);
err_disable_sram_clk:
	if (priv->clk_sram)
		clk_disable_unprepare(priv->clk_sram);
err_disable_bus:
	if (priv->clk_bus)
		clk_disable_unprepare(priv->clk_bus);
err_disable_parent:
	if (priv->clk_parent)
		clk_disable_unprepare(priv->clk_parent);
err_assert_msgbox:
	if (priv->rst_msgbox)
		reset_control_assert(priv->rst_msgbox);
err_assert_sram:
	if (priv->rst_sram)
		reset_control_assert(priv->rst_sram);
err_assert_cfg:
	if (priv->rst_cfg)
		reset_control_assert(priv->rst_cfg);
	return ret;
}

static int sunxi_rproc_unprepare(struct rproc *rproc)
{
	struct sunxi_rproc *priv = rproc->priv;

	/* Symmetrical CCU unwinding */
	if (priv->remap_va) {
		u32 remap_val = readl(priv->remap_va);

		remap_val &= ~SUNXI_REMAP_SRAMA3_2_BIT;
		writel(remap_val, priv->remap_va);
	}

	if (priv->clk_core)
		clk_disable_unprepare(priv->clk_core);

	if (priv->clk_msgbox)
		clk_disable_unprepare(priv->clk_msgbox);

	if (priv->clk_sram)
		clk_disable_unprepare(priv->clk_sram);

	if (priv->clk_bus)
		clk_disable_unprepare(priv->clk_bus);

	if (priv->clk_parent)
		clk_disable_unprepare(priv->clk_parent);

	if (priv->rst_msgbox)
		reset_control_assert(priv->rst_msgbox);

	if (priv->rst_sram)
		reset_control_assert(priv->rst_sram);

	if (priv->rst_cfg)
		reset_control_assert(priv->rst_cfg);

	return 0;
}

static int sunxi_rproc_start(struct rproc *rproc)
{
	struct sunxi_rproc *priv = rproc->priv;
	int ret;

	dev_info(priv->dev, "Starting %s core at entry 0x%llx\n",
		 priv->cfg ? priv->cfg->name : "remote", (u64)rproc->bootaddr);

	if (rproc->bootaddr > U32_MAX)
		return -EINVAL;

	/* XuanTie RISC-V boot sequence */
	if (priv->cfg_va) {
		writel((u32)rproc->bootaddr, priv->cfg_va + E906_STA_ADD_REG);
		dev_dbg(priv->dev, "STA_ADD set to 0x%08x\n", (u32)rproc->bootaddr);
	}

	/* Release core reset to begin execution */
	if (priv->rst_core) {
		ret = reset_control_deassert(priv->rst_core);
		if (ret) {
			dev_err(priv->dev, "failed to release core reset: %d\n", ret);
			return ret;
		}
	} else if (priv->rst_cfg) {
		ret = reset_control_deassert(priv->rst_cfg);
		if (ret) {
			dev_err(priv->dev, "failed to release cfg reset: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int sunxi_rproc_stop(struct rproc *rproc)
{
	struct sunxi_rproc *priv = rproc->priv;

	cancel_work_sync(&priv->vq_work);

	dev_info(priv->dev, "Halting %s core...\n",
		 priv->cfg ? priv->cfg->name : "remote");

	if (priv->rst_core)
		reset_control_assert(priv->rst_core);
	else if (priv->rst_cfg)
		reset_control_assert(priv->rst_cfg);

	return 0;
}

static void sunxi_rproc_kick(struct rproc *rproc, int vqid)
{
	struct sunxi_rproc *priv = rproc->priv;
	int ret;

	if (!priv->tx_chan)
		return;

	ret = mbox_send_message(priv->tx_chan, (void *)&vqid);
	if (ret < 0)
		dev_err_ratelimited(priv->dev, "failed to send mailbox kick: %d\n", ret);

	mbox_client_txdone(priv->tx_chan, 0);
}

static void *sunxi_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	struct sunxi_rproc *priv = rproc->priv;

	if (len == 0)
		return NULL;

	/* 1. Dedicated MCU Local SRAM Space 0 (Resource "r_sram" / "sram") */
	if (priv->r_sram_va) {
		/* Host physical address view (e.g., 0x07280000, 0x07200000, 0x00020000) */
		if (da >= priv->r_sram_phys &&
		    (da + len) <= (priv->r_sram_phys + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - priv->r_sram_phys);
		}
		/* Core DA view: 0x40000000 */
		if (da >= 0x40000000 &&
		    (da + len) <= (0x40000000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x40000000);
		}
		/* High SRAM Space 0 fallback views (0x3ff80000 / 0x3ffc0000) */
		if (da >= 0x3ff80000 && (da + len) <= (0x3ff80000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x3ff80000);
		}
		if (da >= 0x3ffc0000 && (da + len) <= (0x3ffc0000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x3ffc0000);
		}
		/* PubSRAM C DA view (0x00020000) */
		if (da >= 0x00020000 && (da + len) <= (0x00020000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x00020000);
		}
	}

	/* 2. Switchable MCU Local SRAM Space 1 ("r_sram1", Core DA 0x40040000) */
	if (priv->r_sram1_va) {
		/* Host physical address view (e.g., 0x072c0000 or 0x07280000) */
		if (da >= priv->r_sram1_phys &&
		    (da + len) <= (priv->r_sram1_phys + priv->r_sram1_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram1_va + (da - priv->r_sram1_phys);
		}
		/* Core DA view: 0x40000000 (Space 1) and 0x40040000 */
		if (da >= 0x40000000 &&
		    (da + len) <= (0x40000000 + priv->r_sram1_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram1_va + (da - 0x40000000);
		}
		if (da >= 0x40040000 &&
		    (da + len) <= (0x40040000 + priv->r_sram1_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram1_va + (da - 0x40040000);
		}
	}

	/* 4. Trace / Reserved Memory (from Device Tree) */
	if (priv->trace_va) {
		if (da >= priv->trace_phys && (da + len) <= (priv->trace_phys + priv->trace_size)) {
			if (is_iomem)
				*is_iomem = false;
			return priv->trace_va + (da - priv->trace_phys);
		}
	}

	/* 5. Boot DRAM Carveout (Resource "dram" - Core/Host 0x40014000) */
	if (priv->dram_va) {
		if (da >= priv->dram_phys && (da + len) <= (priv->dram_phys + priv->dram_size)) {
			if (is_iomem)
				*is_iomem = false;
			return priv->dram_va + (da - priv->dram_phys);
		}
	}

	/*
	 * Return NULL to delegate all DRAM carveouts (vrings, buffers,
	 * code/data placed in DDR) directly to the remoteproc core's
	 * internal carveout table.
	 */
	return NULL;
}

static int sunxi_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret == -EINVAL) {
		dev_dbg(rproc->dev.parent, "no resource table found in ELF\n");
		return 0;
	}
	return ret;
}

static const struct rproc_ops sunxi_rproc_ops = {
	.prepare        = sunxi_rproc_prepare,
	.unprepare      = sunxi_rproc_unprepare,
	.start          = sunxi_rproc_start,
	.stop           = sunxi_rproc_stop,
	.kick           = sunxi_rproc_kick,
	.da_to_va       = sunxi_rproc_da_to_va,
	.get_boot_addr  = rproc_elf_get_boot_addr,
	.load           = rproc_elf_load_segments,
	.parse_fw       = sunxi_rproc_parse_fw,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check   = rproc_elf_sanity_check,
	.coredump       = rproc_coredump,
};

static int sunxi_rproc_register_mem(struct platform_device *pdev, struct rproc *rproc)
{
	struct device *dev = &pdev->dev;
	struct sunxi_rproc *priv = rproc->priv;
	struct resource *res;

	/* 1. Map Optional RISC-V CFG Block ("cfg") */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (res) {
		priv->cfg_phys = res->start;
		priv->cfg_va = devm_ioremap(dev, res->start, resource_size(res));
		if (!priv->cfg_va)
			dev_warn(dev, "failed to map 'cfg' registers\n");
	}

	/* 2. Map Dedicated RISC-V Local SRAM Space 0 ("r_sram" or "sram") */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "r_sram");
	if (!res)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (res) {
		priv->r_sram_phys = res->start;
		priv->r_sram_size = resource_size(res);
		priv->r_sram_va = devm_ioremap_wc(dev, res->start, resource_size(res));
		if (!priv->r_sram_va)
			return -ENOMEM;
	}

	/* 3. Map Switchable RISC-V Local SRAM Space 1 ("r_sram1" / SRAMA3_2) - Optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "r_sram1");
	if (res) {
		priv->r_sram1_phys = res->start;
		priv->r_sram1_size = resource_size(res);
		priv->r_sram1_va = devm_ioremap_wc(dev, res->start, resource_size(res));
		if (!priv->r_sram1_va)
			dev_warn(dev, "failed to map 'r_sram1' resource\n");
	}

	/* 4. Map Remap Control Register ("remap" or "sram-for-cpux") - Optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "remap");
	if (!res)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram-for-cpux");
	if (res) {
		priv->remap_phys = res->start;
		if (resource_size(res) > SUNXI_REMAP_CTRL_OFFSET && (res->start & 0xfff) == 0) {
			void __iomem *base = devm_ioremap(dev, res->start, resource_size(res));

			if (base)
				priv->remap_va = base + SUNXI_REMAP_CTRL_OFFSET;
		} else {
			priv->remap_va = devm_ioremap(dev, res->start, resource_size(res));
		}
		if (!priv->remap_va)
			dev_warn(dev, "failed to map 'remap' register\n");
	}

	/* 4b. Map Boot DRAM Carveout ("dram" or memory-region "vram"/"dram") */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dram");
	if (res) {
		priv->dram_phys = res->start;
		priv->dram_size = resource_size(res);
		priv->dram_va = devm_memremap(dev, res->start, resource_size(res), MEMREMAP_WB);
		if (!priv->dram_va)
			priv->dram_va = devm_ioremap_wc(dev, res->start, resource_size(res));
		if (!priv->dram_va)
			dev_warn(dev, "failed to map 'dram' resource\n");
	} else if (dev->of_node) {
		int idx = of_property_match_string(dev->of_node, "memory-region-names", "vram");

		if (idx < 0)
			idx = of_property_match_string(dev->of_node, "memory-region-names", "dram");
		if (idx >= 0) {
			struct device_node *rmem_np;

			rmem_np = of_parse_phandle(dev->of_node, "memory-region", idx);
			if (rmem_np) {
				struct resource r;

				if (of_address_to_resource(rmem_np, 0, &r) == 0) {
					priv->dram_phys = r.start;
					priv->dram_size = resource_size(&r);
					priv->dram_va = devm_memremap(dev, r.start,
								      resource_size(&r),
								      MEMREMAP_WB);
					if (!priv->dram_va)
						priv->dram_va = devm_ioremap_wc(dev, r.start,
										resource_size(&r));
					dev_info(dev, "mapped DT vram reserved-memory %pa+%zu\n",
						 &priv->dram_phys, priv->dram_size);
				}
				of_node_put(rmem_np);
			}
		}
	}

	dev_info(dev, "Memory resources: r_sram=%s, r_sram1=%s, remap=%s, cfg=%s, dram=%s\n",
		 priv->r_sram_va ? "yes" : "no",
		 priv->r_sram1_va ? "yes" : "no",
		 priv->remap_va ? "yes" : "no",
		 priv->cfg_va ? "yes" : "no",
		 priv->dram_va ? "yes" : "no");

	/* 6. Map Trace Buffer / Reserved Memory from Device Tree */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "trace");
	if (res) {
		priv->trace_phys = res->start;
		priv->trace_size = resource_size(res);
		priv->trace_va = devm_memremap(dev, res->start, resource_size(res), MEMREMAP_WB);
		if (!priv->trace_va)
			priv->trace_va = devm_ioremap_wc(dev, res->start, resource_size(res));
		dev_info(dev, "mapped 'trace' mmio resource %pa+%zu\n",
			 &priv->trace_phys, priv->trace_size);
	} else if (dev->of_node) {
		struct device_node *rmem_np;
		int idx;

		/* Check memory-region phandle only if explicitly named 'trace' */
		idx = of_property_match_string(dev->of_node, "memory-region-names", "trace");
		if (idx >= 0) {
			rmem_np = of_parse_phandle(dev->of_node, "memory-region", idx);
			if (rmem_np) {
				struct resource r;

				if (of_address_to_resource(rmem_np, 0, &r) == 0) {
					priv->trace_phys = r.start;
					priv->trace_size = resource_size(&r);
					priv->trace_va = devm_memremap(dev, r.start,
								       resource_size(&r),
								       MEMREMAP_WB);
					if (!priv->trace_va)
						priv->trace_va = devm_ioremap_wc(dev, r.start,
										 resource_size(&r));
					dev_info(dev, "mapped DT trace reserved-memory %pa+%zu\n",
						 &priv->trace_phys, priv->trace_size);
				}
				of_node_put(rmem_np);
			}
		}
	}

	return 0;
}

static int sunxi_rproc_parse_memory_regions(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct sunxi_rproc *priv = rproc->priv;
	int num_rmems;
	int i;

	if (!np)
		return 0;

	num_rmems = of_count_phandle_with_args(np, "memory-region", NULL);
	if (num_rmems <= 0)
		return 0;

	/* Bind default DMA pool for dynamic allocations (e.g. vdev0 vrings/buffers) */
	if (of_reserved_mem_device_init(dev) == 0)
		priv->has_reserved_mem = true;
	else
		dev_dbg(dev, "no dedicated DMA pool assigned from reserved-memory\n");

	/* Register all reserved-memory regions as formal remoteproc carveouts */
	for (i = 0; i < num_rmems; i++) {
		struct device_node *rmem_np;
		struct resource res;
		const char *name = NULL;
		struct rproc_mem_entry *mem;
		void *va;

		rmem_np = of_parse_phandle(np, "memory-region", i);
		if (!rmem_np)
			continue;

		if (of_address_to_resource(rmem_np, 0, &res)) {
			of_node_put(rmem_np);
			continue;
		}

		of_property_read_string_index(np, "memory-region-names", i, &name);
		if (!name)
			name = rmem_np->name;

		if (name && (strstr(name, "trace") || of_node_name_eq(rmem_np, "trace"))) {
			priv->trace_phys = res.start;
			priv->trace_size = resource_size(&res);
			priv->trace_va = devm_memremap(dev, res.start, resource_size(&res),
						       MEMREMAP_WB);
			if (!priv->trace_va)
				priv->trace_va = devm_ioremap_wc(dev, res.start,
								 resource_size(&res));
			dev_info(dev, "registered trace carveout %pa+%zu (%s)\n",
				 &priv->trace_phys, priv->trace_size, name);
		} else if (name && (strstr(name, "dram") || strstr(name, "vram"))) {
			priv->dram_phys = res.start;
			priv->dram_size = resource_size(&res);
			priv->dram_va = devm_memremap(dev, res.start, resource_size(&res),
						      MEMREMAP_WB);
			if (!priv->dram_va)
				priv->dram_va = devm_ioremap_wc(dev, res.start,
								resource_size(&res));
			dev_info(dev, "registered dram carveout %pa+%zu (%s)\n",
				 &priv->dram_phys, priv->dram_size, name);
		}

		/* Reuse existing SRAM mapping if region overlaps, else ioremap */
		if (priv->r_sram1_va && res.start == priv->r_sram1_phys)
			va = priv->r_sram1_va;
		else if (priv->r_sram_va && res.start == priv->r_sram_phys)
			va = priv->r_sram_va;
		else
			va = devm_ioremap_wc(dev, res.start, resource_size(&res));

		if (va) {
			mem = rproc_mem_entry_init(dev, va, (dma_addr_t)res.start,
						   resource_size(&res), (u32)res.start,
						   NULL, NULL, "%s", name);
			if (mem) {
				mem->is_iomem = true;
				rproc_add_carveout(rproc, mem);
			}
		}
		of_node_put(rmem_np);
	}

	return 0;
}

static int sunxi_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *fw_name = "riscv-firmware.elf";
	struct sunxi_rproc *priv;
	struct rproc *rproc;
	int crash_irq;
	int ret;

	of_property_read_string(dev->of_node, "firmware-name", &fw_name);

	rproc = devm_rproc_alloc(dev, dev_name(dev), &sunxi_rproc_ops,
				 fw_name, sizeof(*priv));
	if (!rproc) {
		dev_err(dev, "failed to allocate rproc context\n");
		return -ENOMEM;
	}

	priv = rproc->priv;
	priv->rproc = rproc;
	priv->dev = dev;
	priv->cfg = of_device_get_match_data(dev);
	if (!priv->cfg)
		priv->cfg = &sun55i_riscv_cfg;

	/* 1. Common Clock Framework (CCF) Clocks */
	priv->clk_parent = devm_clk_get_optional(dev, "parent");
	if (IS_ERR(priv->clk_parent))
		return dev_err_probe(dev, PTR_ERR(priv->clk_parent),
				     "failed to get 'parent' clock\n");

	priv->clk_bus = devm_clk_get_optional(dev, "bus");
	if (IS_ERR(priv->clk_bus))
		return dev_err_probe(dev, PTR_ERR(priv->clk_bus), "failed to get 'bus' clock\n");

	priv->clk_core = devm_clk_get_optional(dev, "core");
	if (IS_ERR(priv->clk_core))
		return dev_err_probe(dev, PTR_ERR(priv->clk_core), "failed to get 'core' clock\n");

	priv->clk_sram = devm_clk_get_optional(dev, "sram");
	if (IS_ERR(priv->clk_sram))
		return dev_err_probe(dev, PTR_ERR(priv->clk_sram), "failed to get 'sram' clock\n");

	priv->clk_msgbox = devm_clk_get_optional(dev, "msgbox");
	if (IS_ERR(priv->clk_msgbox))
		return dev_err_probe(dev, PTR_ERR(priv->clk_msgbox),
				     "failed to get 'msgbox' clock\n");

	/* 2. Resets */
	priv->rst_core = devm_reset_control_get_optional_exclusive(dev, "core");
	if (IS_ERR(priv->rst_core))
		return dev_err_probe(dev, PTR_ERR(priv->rst_core), "failed to get 'core' reset\n");

	priv->rst_cfg = devm_reset_control_get_optional_exclusive(dev, "cfg");
	if (IS_ERR(priv->rst_cfg))
		return dev_err_probe(dev, PTR_ERR(priv->rst_cfg), "failed to get 'cfg' reset\n");

	priv->rst_sram = devm_reset_control_get_optional_exclusive(dev, "sram");
	if (IS_ERR(priv->rst_sram))
		return dev_err_probe(dev, PTR_ERR(priv->rst_sram), "failed to get 'sram' reset\n");

	priv->rst_msgbox = devm_reset_control_get_optional_exclusive(dev, "msgbox");
	if (IS_ERR(priv->rst_msgbox))
		return dev_err_probe(dev, PTR_ERR(priv->rst_msgbox),
				     "failed to get 'msgbox' reset\n");

	/* 3. Memory Windows (TCM, SRAM, CFG) */
	ret = sunxi_rproc_register_mem(pdev, rproc);
	if (ret)
		return ret;

	/* 4. Dynamic Reserved Memory Carveouts & DMA Pools from Device Tree */
	ret = sunxi_rproc_parse_memory_regions(rproc);
	if (ret)
		return ret;

	/* 5. Optional Hardware Crash Notification IRQ */
	crash_irq = platform_get_irq_byname_optional(pdev, "crash");
	if (crash_irq > 0) {
		ret = devm_request_threaded_irq(dev, crash_irq, NULL,
						sunxi_rproc_crash_handler,
						IRQF_ONESHOT, "sunxi-rproc-crash",
						rproc);
		if (ret)
			dev_warn(dev, "failed to request crash IRQ %d: %d\n", crash_irq, ret);
	}

	/* 6. Mailbox IPC Client */
	priv->cl.dev = dev;
	priv->cl.rx_callback = sunxi_rproc_mb_rx_callback;
	priv->cl.tx_block = false;
	priv->cl.knows_txdone = true;

	/*
	 * If the hardware mailbox is assigned to userspace (generic-uio) or
	 * lacks #mbox-cells, run RemoteProc in standalone mode.
	 */
	if (dev->of_node) {
		struct device_node *mb_node = of_parse_phandle(dev->of_node, "mboxes", 0);

		if (mb_node) {
			if (of_device_is_compatible(mb_node, "generic-uio") ||
			    !of_property_read_bool(mb_node, "#mbox-cells")) {
				dev_info(dev, "Mailbox assigned to UIO; running standalone mode\n");
				of_node_put(mb_node);
				goto skip_mbox;
			}
			of_node_put(mb_node);
		}
	}

	priv->tx_chan = mbox_request_channel_byname(&priv->cl, "tx");
	if (IS_ERR(priv->tx_chan)) {
		if (PTR_ERR(priv->tx_chan) == -EPROBE_DEFER) {
			ret = -EPROBE_DEFER;
			goto err_mem_release;
		}
		dev_info(dev, "no tx mailbox channel configured; running standalone mode\n");
		priv->tx_chan = NULL;
	}

	if (priv->tx_chan) {
		priv->rx_chan = mbox_request_channel_byname(&priv->cl, "rx");
		if (IS_ERR(priv->rx_chan)) {
			if (PTR_ERR(priv->rx_chan) == -EPROBE_DEFER) {
				ret = -EPROBE_DEFER;
				goto err_mbox_release;
			}
			dev_info(dev, "no rx mailbox channel configured\n");
			priv->rx_chan = NULL;
		}
	}

skip_mbox:
	INIT_WORK(&priv->vq_work, sunxi_rproc_vq_work);
	platform_set_drvdata(pdev, rproc);

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "failed to register rproc device: %d\n", ret);
		goto err_mbox_release;
	}

	dev_info(dev, "Allwinner %s remoteproc registered (%s)\n",
		 priv->cfg ? priv->cfg->name : "remote", fw_name);
	return 0;

err_mbox_release:
	cancel_work_sync(&priv->vq_work);
	if (priv->rx_chan)
		mbox_free_channel(priv->rx_chan);
	if (priv->tx_chan)
		mbox_free_channel(priv->tx_chan);
err_mem_release:
	if (priv->has_reserved_mem)
		of_reserved_mem_device_release(dev);
	return ret;
}

static void sunxi_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct sunxi_rproc *priv = rproc->priv;

	cancel_work_sync(&priv->vq_work);
	rproc_del(rproc);

	if (priv->rx_chan)
		mbox_free_channel(priv->rx_chan);
	if (priv->tx_chan)
		mbox_free_channel(priv->tx_chan);

	if (priv->has_reserved_mem)
		of_reserved_mem_device_release(&pdev->dev);
}

static const struct of_device_id sunxi_rproc_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-rproc", .data = &sun55i_riscv_cfg },
	{ .compatible = "allwinner,sun55i-a527-rproc", .data = &sun55i_riscv_cfg },
	{ .compatible = "allwinner,sun55i-t527-rproc", .data = &sun55i_riscv_cfg },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sunxi_rproc_of_match);

static struct platform_driver sunxi_rproc_driver = {
	.probe = sunxi_rproc_probe,
	.remove = sunxi_rproc_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = sunxi_rproc_of_match,
	},
};
module_platform_driver(sunxi_rproc_driver);

MODULE_AUTHOR("Allwinner Technology Co., Ltd.");
MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("Allwinner XuanTie E906/E907 RISC-V Remoteproc Driver");
MODULE_LICENSE("GPL");
