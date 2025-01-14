// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2025 Loongson Technology Corporation Limited */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/ls6000se.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/*
 * The Loongson Security Module provides the control for hardware
 * encryption acceleration child devices. The SE framework is
 * shown as follows:
 *
 *                   +------------+
 *                   |    CPU     |
 *                   +------------+
 *			^	^
 *	            DMA |	| IRQ
 *			v	v
 *        +-----------------------------------+
 *        |     Loongson Security Module      |
 *        +-----------------------------------+
 *             ^                ^
 *    channel1 |       channel2 |
 *             v                v
 *        +-----------+    +----------+
 *        | sub-dev1  |    | sub-dev2 |  ..... Max sub-dev31
 *        +-----------+    +----------+
 *
 * The CPU cannot directly communicate with SE's sub devices,
 * but sends commands to SE, which processes the commands and
 * sends them to the corresponding sub devices.
 */

struct loongson_se {
	void __iomem *base;
	u32 version;
	spinlock_t dev_lock;
	struct completion cmd_completion;

	/* dma memory */
	void *mem_base;
	int mem_map_pages;
	unsigned long *mem_map;

	/* channel */
	struct mutex ch_init_lock;
	struct lsse_ch chs[SE_CH_MAX];
};

union se_request {
	u32 info[8];
	struct se_cmd {
		u32 cmd;
		u32 info[7];
	} req;
	struct se_res {
		u32 cmd;
		u32 cmd_ret;
		u32 info[6];
	} res;
};

static inline u32 se_readl(struct loongson_se *se, u32 off)
{
	return readl(se->base + off);
}

static inline void se_writel(struct loongson_se *se, u32 val, u32 off)
{
	writel(val, se->base + off);
}

static void se_enable_int_locked(struct loongson_se *se, u32 int_bit)
{
	u32 tmp;

	tmp = se_readl(se, SE_S2LINT_EN);
	tmp |= int_bit;
	se_writel(se, tmp, SE_S2LINT_EN);
}

static void se_disable_int(struct loongson_se *se, u32 int_bit)
{
	unsigned long flag;
	u32 tmp;

	spin_lock_irqsave(&se->dev_lock, flag);

	tmp = se_readl(se, SE_S2LINT_EN);
	tmp &= ~(int_bit);
	se_writel(se, tmp, SE_S2LINT_EN);

	spin_unlock_irqrestore(&se->dev_lock, flag);
}

static int se_poll(struct loongson_se *se, u32 int_bit)
{
	u32 status;
	int err;

	spin_lock_irq(&se->dev_lock);

	se_enable_int_locked(se, int_bit);
	se_writel(se, int_bit, SE_L2SINT_SET);
	err = readl_relaxed_poll_timeout_atomic(se->base + SE_L2SINT_STAT, status,
						!(status & int_bit), 1, 10000);

	spin_unlock_irq(&se->dev_lock);

	return err;
}

static int se_send_requeset(struct loongson_se *se, union se_request *req)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(req->info); i++)
		se_writel(se, req->info[i], SE_DATA_S + i * 4);

	return se_poll(se, SE_INT_SETUP);
}

/*
 * Called by SE's child device driver.
 * Send a request to the corresponding device.
 */
int se_send_ch_requeset(struct lsse_ch *ch)
{
	return se_poll(ch->se, ch->int_bit);
}
EXPORT_SYMBOL_GPL(se_send_ch_requeset);

static int se_get_res(struct loongson_se *se, u32 cmd, union se_request *res)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(res->info); i++)
		res->info[i] = se_readl(se, SE_DATA_L + i * 4);

	if (res->res.cmd != cmd)
		return -EFAULT;

	return 0;
}

static int se_send_genl_cmd(struct loongson_se *se, union se_request *req)
{
	int err;

	err = se_send_requeset(se, req);
	if (err)
		return err;

	if (!wait_for_completion_timeout(&se->cmd_completion, HZ))
		return -ETIME;

	return se_get_res(se, req->req.cmd, req);
}

static int se_set_msg(struct lsse_ch *ch)
{
	struct loongson_se *se = ch->se;
	union se_request req;

	req.req.cmd = SE_CMD_SETMSG;
	req.req.info[0] = ch->id;
	req.req.info[1] = ch->smsg - se->mem_base;
	req.req.info[2] = ch->msg_size;

	return se_send_genl_cmd(se, &req);
}

static irqreturn_t se_irq(int irq, void *dev_id)
{
	struct loongson_se *se = (struct loongson_se *)dev_id;
	struct lsse_ch *ch;
	u32 int_status;
	int id;

	int_status = se_readl(se, SE_S2LINT_STAT);
	se_disable_int(se, int_status);
	if (int_status & SE_INT_SETUP) {
		complete(&se->cmd_completion);
		int_status &= ~SE_INT_SETUP;
		se_writel(se, SE_INT_SETUP, SE_S2LINT_CL);
	}

	while (int_status) {
		id = __ffs(int_status);

		ch = &se->chs[id];
		if (ch->complete)
			ch->complete(ch);
		int_status &= ~BIT(id);
		se_writel(se, BIT(id), SE_S2LINT_CL);
	}

	return IRQ_HANDLED;
}

static int se_init_hw(struct loongson_se *se, dma_addr_t addr, int size)
{
	union se_request req;
	int err;

	/* Start engine */
	req.req.cmd = SE_CMD_START;
	err = se_send_genl_cmd(se, &req);
	if (err)
		return err;

	/* Get Version */
	req.req.cmd = SE_CMD_GETVER;
	err = se_send_genl_cmd(se, &req);
	if (err)
		return err;
	se->version = req.res.info[0];

	/* Setup dma memory */
	req.req.cmd = SE_CMD_SETBUF;
	req.req.info[0] = addr & 0xffffffff;
	req.req.info[1] = addr >> 32;

	return se_send_genl_cmd(se, &req);
}

/*
 * se_init_ch() - Init the channel used by child device.
 *
 * Allocate dma memory as agreed upon with SE on SE probe,
 * and register the callback function when the data processing
 * in this channel is completed.
 */
struct lsse_ch *se_init_ch(struct device *dev, int id, int data_size, int msg_size,
			   void *priv, void (*complete)(struct lsse_ch *se_ch))
{
	struct loongson_se *se = dev_get_drvdata(dev);
	struct lsse_ch *ch;
	int data_first, data_nr;
	int msg_first, msg_nr;

	mutex_lock(&se->ch_init_lock);

	ch = &se->chs[id];
	ch->se = se;
	ch->id = id;
	ch->int_bit = BIT(id);

	data_nr = round_up(data_size, PAGE_SIZE) / PAGE_SIZE;
	data_first = bitmap_find_next_zero_area(se->mem_map, se->mem_map_pages,
						0, data_nr, 0);
	if (data_first >= se->mem_map_pages) {
		ch = NULL;
		goto out_unlock;
	}

	bitmap_set(se->mem_map, data_first, data_nr);
	ch->off = data_first * PAGE_SIZE;
	ch->data_buffer = se->mem_base + ch->off;
	ch->data_size = data_size;

	msg_nr = round_up(msg_size, PAGE_SIZE) / PAGE_SIZE;
	msg_first = bitmap_find_next_zero_area(se->mem_map, se->mem_map_pages,
					       0, msg_nr, 0);
	if (msg_first >= se->mem_map_pages) {
		ch = NULL;
		goto out_unlock;
	}

	bitmap_set(se->mem_map, msg_first, msg_nr);
	ch->smsg = se->mem_base + msg_first * PAGE_SIZE;
	ch->rmsg = ch->smsg + msg_size / 2;
	ch->msg_size = msg_size;
	ch->complete = complete;
	ch->priv = priv;
	ch->version = se->version;

	if (se_set_msg(ch))
		ch = NULL;

out_unlock:
	mutex_unlock(&se->ch_init_lock);

	return ch;
}
EXPORT_SYMBOL_GPL(se_init_ch);

static const struct mfd_cell se_devs[] = {
	{ .name = "ls6000se-sdf" },
	{ .name = "ls6000se-rng" },
};

static int loongson_se_probe(struct platform_device *pdev)
{
	struct loongson_se *se;
	struct device *dev = &pdev->dev;
	int nr_irq, irq, err, size;
	dma_addr_t paddr;

	se = devm_kmalloc(dev, sizeof(*se), GFP_KERNEL);
	if (!se)
		return -ENOMEM;
	dev_set_drvdata(dev, se);
	init_completion(&se->cmd_completion);
	spin_lock_init(&se->dev_lock);
	mutex_init(&se->ch_init_lock);
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (device_property_read_u32(dev, "dmam_size", &size))
		return -ENODEV;
	size = roundup_pow_of_two(size);
	se->mem_base = dmam_alloc_coherent(dev, size, &paddr, GFP_KERNEL);
	if (!se->mem_base)
		return -ENOMEM;
	se->mem_map_pages = size / PAGE_SIZE;
	se->mem_map = devm_bitmap_zalloc(dev, se->mem_map_pages, GFP_KERNEL);
	if (!se->mem_map)
		return -ENOMEM;

	se->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(se->base))
		return PTR_ERR(se->base);

	nr_irq = platform_irq_count(pdev);
	if (nr_irq <= 0)
		return -ENODEV;
	while (nr_irq) {
		irq = platform_get_irq(pdev, --nr_irq);
		/* Use the same interrupt handler address.
		 * Determine which irq it is accroding
		 * SE_S2LINT_STAT register.
		 */
		err = devm_request_irq(dev, irq, se_irq, 0, "ls6000se", se);
		if (err)
			dev_err(dev, "failed to request irq: %d\n", irq);
	}

	err = se_init_hw(se, paddr, size);
	if (err)
		return err;

	return devm_mfd_add_devices(dev, 0, se_devs, ARRAY_SIZE(se_devs),
				    NULL, 0, NULL);
}

static const struct acpi_device_id loongson_se_acpi_match[] = {
	{"LOON0011", 0},
	{}
};
MODULE_DEVICE_TABLE(acpi, loongson_se_acpi_match);

static struct platform_driver loongson_se_driver = {
	.probe   = loongson_se_probe,
	.driver  = {
		.name  = "ls6000se",
		.acpi_match_table = loongson_se_acpi_match,
	},
};
module_platform_driver(loongson_se_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yinggang Gu <guyinggang@loongson.cn>");
MODULE_AUTHOR("Qunqin Zhao <zhaoqunqin@loongson.cn>");
MODULE_DESCRIPTION("Loongson Security Module driver");
