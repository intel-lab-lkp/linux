// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/devcoredump.h>
#include <linux/firmware.h>
#include <linux/of_reserved_mem.h>

#include "airoha_eth.h"

#define NPU_EN7581_FIRMWARE_DATA		"airoha/en7581_npu_data.bin"
#define NPU_EN7581_FIRMWARE_RV32		"airoha/en7581_npu_rv32.bin"
#define NPU_EN7581_FIRMWARE_RV32_MAX_SIZE	0x200000
#define NPU_EN7581_FIRMWARE_DATA_MAX_SIZE	0x10000
#define NPU_DUMP_SIZE				512

#define REG_NPU_LOCAL_SRAM		0x0

#define NPU_PC_BASE_ADDR		0x305000
#define REG_PC_DBG(_n)			(0x305000 + ((_n) * 0x100))

#define NPU_CLUSTER_BASE_ADDR		0x306000

#define REG_CR_BOOT_TRIGGER		(NPU_CLUSTER_BASE_ADDR + 0x000)
#define REG_CR_BOOT_CONFIG		(NPU_CLUSTER_BASE_ADDR + 0x004)
#define REG_CR_BOOT_BASE(_n)		(NPU_CLUSTER_BASE_ADDR + 0x020 + ((_n) << 2))

#define NPU_MBOX_BASE_ADDR		0x30c000

#define REG_CR_MBOX_INT_STATUS		(NPU_MBOX_BASE_ADDR + 0x000)
#define MBOX_INT_STATUS_MASK		BIT(8)

#define REG_CR_MBOX_INT_MASK(_n)	(NPU_MBOX_BASE_ADDR + 0x004 + ((_n) << 2))
#define REG_CR_MBQ0_CTRL(_n)		(NPU_MBOX_BASE_ADDR + 0x030 + ((_n) << 2))
#define REG_CR_MBQ8_CTRL(_n)		(NPU_MBOX_BASE_ADDR + 0x0b0 + ((_n) << 2))
#define REG_CR_NPU_MIB(_n)		(NPU_MBOX_BASE_ADDR + 0x140 + ((_n) << 2))

#define NPU_TIMER_BASE_ADDR		0x310100
#define REG_WDT_TIMER_CTRL(_n)		(NPU_TIMER_BASE_ADDR + ((_n) * 0x100))
#define WDT_EN_MASK			BIT(25)
#define WDT_INTR_MASK			BIT(21)

enum {
	NPU_OP_SET = 1,
	NPU_OP_SET_NO_WAIT,
	NPU_OP_GET,
	NPU_OP_GET_NO_WAIT,
};

enum {
	NPU_FUNC_WIFI,
	NPU_FUNC_TUNNEL,
	NPU_FUNC_NOTIFY,
	NPU_FUNC_DBA,
	NPU_FUNC_TR471,
	NPU_FUNC_PPE,
};

enum {
	NPU_MBOX_ERROR,
	NPU_MBOX_SUCCESS,
};

enum {
	PPE_FUNC_SET_WAIT,
	PPE_FUNC_SET_WAIT_HWNAT_INIT,
	PPE_FUNC_SET_WAIT_HWNAT_DEINIT,
	PPE_FUNC_SET_WAIT_API,
};

enum {
	PPE2_SRAM_SET_ENTRY,
	PPE_SRAM_SET_ENTRY,
	PPE_SRAM_SET_VAL,
	PPE_SRAM_RESET_VAL,
};

enum {
	QDMA_WAN_ETHER = 1,
	QDMA_WAN_PON_XDSL,
};

struct npu_mbox_metadata {
	union {
		struct {
			u16 wait_rsp:1;
			u16 done:1;
			u16 status:3;
			u16 static_buf:1;
			u16 rsv:5;
			u16 func_id:4;
		};
		u16 data;
	};
};

#define PPE_TYPE_L2B_IPV4	2
#define PPE_TYPE_L2B_IPV4_IPV6	3

struct ppe_mbox_data {
	u32 func_type;
	u32 func_id;
	union {
		struct {
			u8 cds;
			u8 xpon_hal_api;
			u8 wan_xsi;
			u8 ct_joyme4;
			int ppe_type;
			int wan_mode;
			int wan_sel;
		} init_info;
		struct {
			int func_id;
			u32 size;
			u32 data;
		} set_info;
	};
};

static u32 airoha_npu_rr(struct airoha_npu *npu, u32 reg)
{
	return readl(npu->base + reg);
}

static void airoha_npu_wr(struct airoha_npu *npu, u32 reg, u32 val)
{
	writel(val, npu->base + reg);
}

static u32 airoha_npu_rmw(struct airoha_npu *npu, u32 reg, u32 mask, u32 val)
{
	val |= airoha_npu_rr(npu, reg) & ~mask;
	airoha_npu_wr(npu, reg, val);

	return val;
}

static int airoha_npu_send_msg(struct airoha_npu *npu, int func_id,
			       void *p, int size)
{
	struct device *dev = &npu->pdev->dev;
	struct npu_mbox_metadata meta = {
		.wait_rsp = 1,
		.func_id = func_id,
	};
	u16 core = 0; /* FIXME */
	u32 val, offset = core << 4;
	dma_addr_t dma_addr;
	void *addr;
	int ret;

	addr = kzalloc(size, GFP_ATOMIC | GFP_DMA);
	if (!addr)
		return -ENOMEM;

	memcpy(addr, p, size);
	dma_addr = dma_map_single(dev, addr, size, DMA_TO_DEVICE);
	ret = dma_mapping_error(dev, dma_addr);
	if (ret)
		goto out;

	spin_lock_bh(&npu->cores[core].lock);

	airoha_npu_wr(npu, REG_CR_MBQ0_CTRL(0) + offset, dma_addr);
	airoha_npu_wr(npu, REG_CR_MBQ0_CTRL(1) + offset, size);
	val = airoha_npu_rr(npu, REG_CR_MBQ0_CTRL(2) + offset);
	airoha_npu_wr(npu, REG_CR_MBQ0_CTRL(2) + offset, val + 1);
	airoha_npu_wr(npu, REG_CR_MBQ0_CTRL(3) + offset, meta.data);

	ret = read_poll_timeout_atomic(airoha_npu_rr, meta.data, meta.done,
				       100, 100 * MSEC_PER_SEC, false, npu,
				       REG_CR_MBQ0_CTRL(3) + offset);
	if (!ret)
		ret = meta.status == NPU_MBOX_SUCCESS ? 0 : -EINVAL;

	spin_unlock_bh(&npu->cores[core].lock);

	dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE);
out:
	kfree(addr);

	return ret;
}

static int airoha_npu_run_firmware(struct airoha_npu *npu, struct reserved_mem *rmem)
{
	struct device *dev = &npu->pdev->dev;
	const struct firmware *fw;
	void __iomem *addr;
	int ret;

	ret = request_firmware(&fw, NPU_EN7581_FIRMWARE_RV32, dev);
	if (ret)
		return ret;

	if (fw->size > NPU_EN7581_FIRMWARE_RV32_MAX_SIZE) {
		dev_err(dev, "%s: fw size too overlimit (%zu)\n",
			NPU_EN7581_FIRMWARE_RV32, fw->size);
		ret = -E2BIG;
		goto out;
	}

	addr = devm_ioremap(dev, rmem->base, rmem->size);
	if (!addr) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy_toio(addr, fw->data, fw->size);
	release_firmware(fw);

	ret = request_firmware(&fw, NPU_EN7581_FIRMWARE_DATA, dev);
	if (ret)
		return ret;

	if (fw->size > NPU_EN7581_FIRMWARE_DATA_MAX_SIZE) {
		dev_err(dev, "%s: fw size too overlimit (%zu)\n",
			NPU_EN7581_FIRMWARE_DATA, fw->size);
		ret = -E2BIG;
		goto out;
	}

	memcpy_toio(npu->base + REG_NPU_LOCAL_SRAM, fw->data, fw->size);
out:
	release_firmware(fw);

	return ret;
}

static irqreturn_t airoha_npu_mbox_handler(int irq, void *npu_instance)
{
	struct airoha_npu *npu = npu_instance;
	struct npu_mbox_metadata meta;

	/* clear mbox interrupt status */
	airoha_npu_wr(npu, REG_CR_MBOX_INT_STATUS, MBOX_INT_STATUS_MASK);

	/* acknowledge npu */
	meta.data = airoha_npu_rr(npu, REG_CR_MBQ8_CTRL(3));
	meta.status = 0;
	meta.done = 1;
	airoha_npu_wr(npu, REG_CR_MBQ8_CTRL(3), meta.data);

	return IRQ_HANDLED;
}

int airoha_npu_ppe_init(struct airoha_npu *npu)
{
	struct ppe_mbox_data ppe_data = {
		.func_type = NPU_OP_SET,
		.func_id = PPE_FUNC_SET_WAIT_HWNAT_INIT,
		.init_info = {
			.ppe_type = PPE_TYPE_L2B_IPV4_IPV6,
			.wan_mode = QDMA_WAN_ETHER,
		},
	};

	return airoha_npu_send_msg(npu, NPU_FUNC_PPE, &ppe_data,
				   sizeof(struct ppe_mbox_data));
}

int airoha_npu_ppe_deinit(struct airoha_npu *npu)
{
	struct ppe_mbox_data ppe_data = {
		.func_type = NPU_OP_SET,
		.func_id = PPE_FUNC_SET_WAIT_HWNAT_DEINIT,
	};

	return airoha_npu_send_msg(npu, NPU_FUNC_PPE, &ppe_data,
				   sizeof(struct ppe_mbox_data));
}

int airoha_npu_flush_ppe_sram_entries(struct airoha_npu *npu,
				      struct airoha_ppe *ppe)
{
	struct ppe_mbox_data ppe_data = {
		.func_type = NPU_OP_SET,
		.func_id = PPE_FUNC_SET_WAIT_API,
		.set_info = {
			.func_id = PPE_SRAM_RESET_VAL,
			.data = ppe->foe_dma,
			.size = PPE_SRAM_NUM_ENTRIES,
		},
	};
	int i, sram_num_entries = PPE_SRAM_NUM_ENTRIES;
	struct airoha_foe_entry *hwe = ppe->foe;

	if (airoha_ppe2_is_enabled(ppe->eth))
		sram_num_entries = sram_num_entries / 2;

	for (i = 0; i < sram_num_entries; i++)
		memset(&hwe[i], 0, sizeof(*hwe));

	return airoha_npu_send_msg(npu, NPU_FUNC_PPE, &ppe_data,
				   sizeof(struct ppe_mbox_data));
}

static void airoha_npu_wdt_work(struct work_struct *work)
{
	struct airoha_npu_core *core;
	struct airoha_npu *npu;
	void *dump;
	int c;

	core = container_of(work, struct airoha_npu_core, wdt_work);
	npu = core->npu;

	dump = vzalloc(NPU_DUMP_SIZE);
	if (!dump)
		return;

	c = core - &npu->cores[0];
	snprintf(dump, NPU_DUMP_SIZE, "PC: %08x SP: %08x LR: %08x\n",
		 airoha_npu_rr(npu, REG_PC_DBG(c)),
		 airoha_npu_rr(npu, REG_PC_DBG(c) + 0x4),
		 airoha_npu_rr(npu, REG_PC_DBG(c) + 0x8));

	dev_coredumpv(&npu->pdev->dev, dump, NPU_DUMP_SIZE, GFP_KERNEL);
}

static irqreturn_t airoha_npu_wdt_handler(int irq, void *core_instance)
{
	struct airoha_npu_core *core = core_instance;
	struct airoha_npu *npu = core->npu;
	int c = core - &npu->cores[0];
	u32 val;

	airoha_npu_rmw(npu, REG_WDT_TIMER_CTRL(c), 0, WDT_INTR_MASK);
	val = airoha_npu_rr(npu, REG_WDT_TIMER_CTRL(c));
	if (FIELD_GET(WDT_EN_MASK, val))
		schedule_work(&core->wdt_work);

	return IRQ_HANDLED;
}

struct airoha_npu *airoha_npu_init(struct airoha_eth *eth)
{
	struct reserved_mem *rmem;
	int i, irq, err = -ENODEV;
	struct airoha_npu *npu;
	struct device_node *np;

	npu = devm_kzalloc(eth->dev, sizeof(*npu), GFP_KERNEL);
	if (!npu)
		return ERR_PTR(-ENOMEM);

	npu->np = of_parse_phandle(eth->dev->of_node, "airoha,npu", 0);
	if (!npu->np)
		return ERR_PTR(-ENODEV);

	npu->pdev = of_find_device_by_node(npu->np);
	if (!npu->pdev)
		goto error_of_node_put;

	get_device(&npu->pdev->dev);

	npu->base = devm_platform_ioremap_resource(npu->pdev, 0);
	if (IS_ERR(npu->base))
		goto error_put_dev;

	np = of_parse_phandle(npu->np, "memory-region", 0);
	if (!np)
		goto error_put_dev;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);

	if (!rmem)
		goto error_put_dev;

	irq = platform_get_irq(npu->pdev, 0);
	if (irq < 0) {
		err = irq;
		goto error_put_dev;
	}

	err = devm_request_irq(&npu->pdev->dev, irq, airoha_npu_mbox_handler,
			       IRQF_SHARED, "airoha-npu-mbox", npu);
	if (err)
		goto error_put_dev;

	for (i = 0; i < ARRAY_SIZE(npu->cores); i++) {
		struct airoha_npu_core *core = &npu->cores[i];

		spin_lock_init(&core->lock);
		core->npu = npu;

		irq = platform_get_irq(npu->pdev, i + 1);
		if (irq < 0) {
			err = irq;
			goto error_put_dev;
		}

		err = devm_request_irq(&npu->pdev->dev, irq,
				       airoha_npu_wdt_handler, IRQF_SHARED,
				       "airoha-npu-wdt", core);
		if (err)
			goto error_put_dev;

		INIT_WORK(&core->wdt_work, airoha_npu_wdt_work);
	}

	if (dma_set_coherent_mask(&npu->pdev->dev, 0xbfffffff))
		dev_err(&npu->pdev->dev,
			"failed coherent DMA configuration\n");

	err = airoha_npu_run_firmware(npu, rmem);
	if (err)
		goto error_put_dev;

	airoha_npu_wr(npu, REG_CR_NPU_MIB(10),
		      rmem->base + NPU_EN7581_FIRMWARE_RV32_MAX_SIZE);
	airoha_npu_wr(npu, REG_CR_NPU_MIB(11), 0x40000); /* SRAM 256K */
	airoha_npu_wr(npu, REG_CR_NPU_MIB(12), 0);
	airoha_npu_wr(npu, REG_CR_NPU_MIB(21), 1);
	msleep(100);

	/* setting booting address */
	for (i = 0; i < AIROHA_NPU_NUM_CORES; i++)
		airoha_npu_wr(npu, REG_CR_BOOT_BASE(i), rmem->base);
	usleep_range(1000, 2000);

	/* enable NPU cores */
	/* do not start core3 since it is used for WiFi offloading */
	airoha_npu_wr(npu, REG_CR_BOOT_CONFIG, 0xf7);
	airoha_npu_wr(npu, REG_CR_BOOT_TRIGGER, 0x1);
	msleep(100);

	return npu;

error_put_dev:
	put_device(&npu->pdev->dev);
error_of_node_put:
	of_node_put(npu->np);

	return ERR_PTR(err);
}

void airoha_npu_deinit(struct airoha_npu *npu)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(npu->cores); i++)
		cancel_work_sync(&npu->cores[i].wdt_work);

	put_device(&npu->pdev->dev);
	of_node_put(npu->np);
}
