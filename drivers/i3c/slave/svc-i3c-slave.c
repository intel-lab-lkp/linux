// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 NXP.
 *
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/i3c/slave.h>
#include <linux/i3c/slave.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/i3c/device.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>

enum i3c_clks {
	PCLK,
	FCLK,
	SCLK,
	MAXCLK,
};

struct svc_i3c_slave {
	struct device *dev;
	void __iomem *regs;
	int irq;
	struct clk_bulk_data clks[MAXCLK];

	struct list_head txq;
	spinlock_t txq_lock; /* protect tx queue */
	struct list_head rxq;
	spinlock_t rxq_lock; /* protect rx queue */
	struct list_head cq;
	spinlock_t cq_lock; /* protect complete queue */

	struct work_struct work;
	struct workqueue_struct *workqueue;

	struct completion dacomplete;
	struct i3c_slave_ctrl_features features;

	spinlock_t ctrl_lock; /* protext access SCTRL register */
};

#define I3C_SCONFIG	0x4
#define   I3C_SCONFIG_SLVENA_MASK	BIT(0)
#define	  I3C_SCONFIG_OFFLINE_MASK	BIT(9)
#define   I3C_SCONFIG_SADDR_MASK	GENMASK(31, 25)

#define I3C_SSTATUS	0x8
#define	  I3C_SSTATUS_STNOTSTOP_MASK	BIT(0)
#define	  I3C_SSTATUS_STOP_MASK		BIT(10)
#define	  I3C_SSTATUS_RX_PEND_MASK	BIT(11)
#define   I3C_SSTATUS_TXNOTFULL_MASK	BIT(12)
#define	  I3C_SSTATUS_DACHG_MASK	BIT(13)
#define	  I3C_SSTATUS_EVDET_MASK	GENMASK(21, 20)
#define	  I3C_SSTATUS_EVDET_ACKED	0x3
#define	  I3C_SSTATUS_IBIDIS_MASK	BIT(24)
#define	  I3C_SSTATUS_HJDIS_MASK	BIT(27)

#define I3C_SCTRL	0xc
#define   I3C_SCTRL_EVENT_MASK		GENMASK(1, 0)
#define	  I3C_SCTRL_EVENT_IBI		0x1
#define	  I3C_SCTRL_EVENT_HOTJOIN	0x3
#define   I3C_SCTRL_EXTDATA_MASK	BIT(3)
#define   I3C_SCTRL_IBIDATA_MASK	GENMASK(15, 8)

#define I3C_SINTSET	0x10
#define I3C_SINTCLR	0x14
#define   I3C_SINT_START	BIT(8)
#define   I3C_SINT_MATCHED	BIT(9)
#define   I3C_SINT_STOP		BIT(10)
#define   I3C_SINT_RXPEND	BIT(11)
#define   I3C_SINT_TXSEND	BIT(12)
#define   I3C_SINT_DACHG	BIT(13)
#define   I3C_SINT_CCC		BIT(14)
#define   I3C_SINT_ERRWARN	BIT(15)
#define   I3C_SINT_DDRMAATCHED	BIT(16)
#define   I3C_SINT_CHANDLED	BIT(17)
#define   I3C_SINT_EVENT	BIT(18)
#define   I3C_SINT_SLVRST	BIT(19)

#define I3C_SDATACTRL	0x2c
#define   I3C_SDATACTRL_RXEMPTY_MASK	BIT(31)
#define   I3C_SDATACTRL_TXFULL_MASK	BIT(30)
#define	  I3C_SDATACTRL_RXCOUNT_MASK	GENMASK(28, 24)
#define	  I3C_SDATACTRL_TXCOUNT_MASK	GENMASK(20, 16)
#define   I3C_SDATACTRL_FLUSHFB_MASK	BIT(1)
#define   I3C_SDATACTRL_FLUSHTB_MASK	BIT(0)

#define I3C_SWDATAB	0x30
#define   I3C_SWDATAB_END_ALSO_MASK	BIT(16)
#define	  I3C_SWDATAB_END_MASK		BIT(8)

#define I3C_SWDATAE	0x34
#define I3C_SRDATAB	0x40

#define I3C_SCAPABILITIES 0x60
#define   I3C_SCAPABILITIES_FIFOTX_MASK     GENMASK(27, 26)
#define   I3C_SCAPABILITIES_FIFORX_MASK     GENMASK(29, 28)

#define I3C_SMAXLIMITS	0x68
#define   I3C_SMAXLIMITS_MAXRD_MASK  GENMASK(11, 0)
#define   I3C_SMAXLIMITS_MAXWR_MASK  GENMASK(27, 16)

#define I3C_SIDPARTNO	0x6c

#define I3C_SIDEXT	0x70
#define	  I3C_SIDEXT_BCR_MASK	GENMASK(23, 16)
#define	  I3C_SIDEXT_DCR_MASK	GENMASK(15, 8)
#define I3C_SVENDORID	0x74

#define I3C_SMAPCTRL0	0x11c
#define	  I3C_SMAPCTRL0_ENA_MASK	BIT(0)
#define   I3C_SMAPCTRL0_DA_MASK	GENMASK(7, 1)

#define I3C_IBIEXT1	0x140
#define   I3C_IBIEXT1_CNT_MASK	GEN_MASK(2, 0)
#define   I3C_IBIEXT1_MAX_MASK	GEN_MASK(4, 6)
#define   I3C_IBIEXT1_EXT1_SHIFT	8
#define   I3C_IBIEXT1_EXT2_SHIFT	16
#define   I3C_IBIEXT1_EXT3_SHIFT	24

#define I3C_IBIEXT2	0x144
#define	  I3C_IBIEXT2_EXT4_SHIFT	0
#define	  I3C_IBIEXT2_EXT5_SHIFT	8
#define	  I3C_IBIEXT2_EXT6_SHIFT	16
#define	  I3C_IBIEXT2_EXT7_SHIFT	24

static int svc_i3c_slave_enable(struct i3c_slave_ctrl *ctrl)
{
	struct svc_i3c_slave *svc;
	u32 val;

	svc = dev_get_drvdata(&ctrl->dev);

	val = readl_relaxed(svc->regs + I3C_SCONFIG);
	val |= I3C_SCONFIG_SLVENA_MASK;
	writel_relaxed(val, svc->regs + I3C_SCONFIG);

	return 0;
}

static int svc_i3c_slave_disable(struct i3c_slave_ctrl *ctrl)
{
	struct svc_i3c_slave *svc;
	u32 val;

	svc = dev_get_drvdata(&ctrl->dev);

	val = readl_relaxed(svc->regs + I3C_SCONFIG);
	val &= ~I3C_SCONFIG_SLVENA_MASK;
	writel_relaxed(val, svc->regs + I3C_SCONFIG);

	return 0;
}

static int svc_i3c_slave_set_config(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func)
{
	struct svc_i3c_slave *svc;
	u32 val;
	u32 wm, rm;

	svc = dev_get_drvdata(&ctrl->dev);

	if (func->static_addr > 0x7F)
		return -EINVAL;

	val = readl_relaxed(svc->regs + I3C_SCONFIG);
	val &= ~I3C_SCONFIG_SLVENA_MASK;
	val |= FIELD_PREP(I3C_SCONFIG_SADDR_MASK, func->static_addr);
	writel_relaxed(val, svc->regs + I3C_SCONFIG);

	if (func->part_id)
		writel_relaxed((func->part_id << 16) |
				((func->instance_id << 12) & GENMASK(15, 12)) |
				(func->ext_id & GENMASK(11, 0)), svc->regs + I3C_SIDPARTNO);

	writel_relaxed(FIELD_PREP(I3C_SIDEXT_BCR_MASK, func->bcr) |
		       FIELD_PREP(I3C_SIDEXT_DCR_MASK, func->dcr),
		       svc->regs + I3C_SIDEXT);

	wm = func->max_write_len == 0 ?
	     FIELD_GET(I3C_SMAXLIMITS_MAXWR_MASK, I3C_SMAXLIMITS_MAXWR_MASK) : func->max_write_len;

	wm = max_t(u32, val, 8);

	rm = func->max_read_len == 0 ?
	     FIELD_GET(I3C_SMAXLIMITS_MAXRD_MASK, I3C_SMAXLIMITS_MAXRD_MASK) : func->max_read_len;
	rm = max_t(u32, val, 16);

	val = FIELD_PREP(I3C_SMAXLIMITS_MAXRD_MASK, rm) | FIELD_PREP(I3C_SMAXLIMITS_MAXWR_MASK, wm);
	writel_relaxed(val, svc->regs + I3C_SMAXLIMITS);

	writel_relaxed(func->vendor_id, svc->regs + I3C_SVENDORID);
	return 0;
}

const struct i3c_slave_ctrl_features *svc_i3c_get_features(struct i3c_slave_ctrl *ctrl)
{
	struct svc_i3c_slave *svc;

	svc = dev_get_drvdata(&ctrl->dev);

	if (!svc)
		return NULL;

	return &svc->features;
}

static void svc_i3c_queue_complete(struct svc_i3c_slave *svc, struct i3c_request *complete)
{
	unsigned long flags;

	spin_lock_irqsave(&svc->cq_lock, flags);
	list_add_tail(&complete->list, &svc->cq);
	spin_unlock_irqrestore(&svc->cq_lock, flags);
	queue_work(svc->workqueue, &svc->work);
}

static void svc_i3c_fill_txfifo(struct svc_i3c_slave *svc)
{
	struct i3c_request *req, *complete = NULL;
	unsigned long flags;
	int val;

	spin_lock_irqsave(&svc->txq_lock, flags);
	while ((!!(req = list_first_entry_or_null(&svc->txq, struct i3c_request, list))) &&
	       !((readl_relaxed(svc->regs + I3C_SDATACTRL) & I3C_SDATACTRL_TXFULL_MASK))) {
		while (!(readl_relaxed(svc->regs + I3C_SDATACTRL)
					& I3C_SDATACTRL_TXFULL_MASK)) {
			val = *(u8 *)(req->buf + req->actual);

			if (req->actual + 1 == req->length)
				writel_relaxed(val, svc->regs + I3C_SWDATAE);
			else
				writel_relaxed(val, svc->regs + I3C_SWDATAB);

			req->actual++;

			if (req->actual == req->length) {
				list_del(&req->list);
				complete = req;
				spin_unlock_irqrestore(&svc->txq_lock, flags);

				svc_i3c_queue_complete(svc, complete);

				spin_lock_irqsave(&svc->txq_lock, flags);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&svc->txq_lock, flags);
}

static int svc_i3c_slave_queue(struct i3c_request *req, gfp_t)
{
	struct svc_i3c_slave *svc;
	struct list_head *q;
	unsigned long flags;
	spinlock_t *lk;

	svc = dev_get_drvdata(&req->ctrl->dev);
	if (!svc)
		return -EINVAL;

	if (req->tx) {
		q = &svc->txq;
		lk = &svc->txq_lock;
	} else {
		q = &svc->rxq;
		lk = &svc->rxq_lock;
	}

	spin_lock_irqsave(lk, flags);
	list_add_tail(&req->list, q);
	spin_unlock_irqrestore(lk, flags);

	if (req->tx)
		svc_i3c_fill_txfifo(svc);

	if (req->tx)
		writel_relaxed(I3C_SINT_TXSEND, svc->regs + I3C_SINTSET);
	else
		writel_relaxed(I3C_SINT_RXPEND, svc->regs + I3C_SINTSET);

	return 0;
}

static int svc_i3c_dequeue(struct i3c_request *req)
{
	struct svc_i3c_slave *svc;
	unsigned long flags;
	spinlock_t *lk;

	svc = dev_get_drvdata(&req->ctrl->dev);
	if (!svc)
		return -EINVAL;

	if (req->tx)
		lk = &svc->txq_lock;
	else
		lk = &svc->rxq_lock;

	spin_lock_irqsave(lk, flags);
	list_del(&req->list);
	spin_unlock_irqrestore(lk, flags);

	return 0;
}

static void svc_i3c_slave_fifo_flush(struct i3c_slave_ctrl *ctrl, bool tx)
{
	struct svc_i3c_slave *svc;
	u32 val;

	svc = dev_get_drvdata(&ctrl->dev);

	val = readl_relaxed(svc->regs + I3C_SDATACTRL);

	val |= tx ? I3C_SDATACTRL_FLUSHTB_MASK : I3C_SDATACTRL_FLUSHFB_MASK;

	writel_relaxed(val, svc->regs + I3C_SDATACTRL);
}

static int
svc_i3c_slave_raise_ibi(struct i3c_slave_ctrl *ctrl, void *p, u8 size)
{
	struct svc_i3c_slave *svc;
	unsigned long flags;
	u8 *ibidata = p;
	u32 ext1 = 0, ext2 = 0;
	u32 val;
	int ret;

	svc = dev_get_drvdata(&ctrl->dev);

	if (size && !p)
		return -EINVAL;

	if (size > 8)
		return -EINVAL;

	val = readl_relaxed(svc->regs + I3C_SSTATUS);
	if (val & I3C_SSTATUS_IBIDIS_MASK)
		return -EINVAL;

	ret = readl_relaxed_poll_timeout(svc->regs + I3C_SCTRL, val,
					 !(val & I3C_SCTRL_EVENT_MASK), 0, 10000);
	if (ret) {
		dev_err(&ctrl->dev, "Timeout when polling for NO event pending");
		val &= ~I3C_SCTRL_EVENT_MASK;
		writel_relaxed(val, svc->regs + I3C_SCTRL);
		return -ENAVAIL;
	}

	spin_lock_irqsave(&svc->ctrl_lock, flags);

	val = readl_relaxed(svc->regs + I3C_SCTRL);

	val &= ~I3C_SCTRL_EVENT_MASK | I3C_SCTRL_IBIDATA_MASK;
	val |= FIELD_PREP(I3C_SCTRL_EVENT_MASK, I3C_SCTRL_EVENT_IBI);

	if (size) {
		val |= FIELD_PREP(I3C_SCTRL_IBIDATA_MASK, *ibidata);
		ibidata++;

		if (size > 1)
			val |= I3C_SCTRL_EXTDATA_MASK;

		size--;
		if (size > 0) {
			ext1 |= (size + 2);
			ext1 |= (*ibidata++) << I3C_IBIEXT1_EXT1_SHIFT;
			size--;
		}

		if (size > 0) {
			ext1 |= (*ibidata++) << I3C_IBIEXT1_EXT2_SHIFT;
			size--;
		}

		if (size > 0) {
			ext1 |= (*ibidata++) << I3C_IBIEXT1_EXT3_SHIFT;
			size--;
		}

		writel_relaxed(ext1, svc->regs + I3C_IBIEXT1);

		if (size > 0) {
			ext2 |= (*ibidata++) << I3C_IBIEXT2_EXT4_SHIFT;
			size--;
		}

		if (size > 0) {
			ext2 |= (*ibidata++) << I3C_IBIEXT2_EXT5_SHIFT;
			size--;
		}

		if (size > 0) {
			ext2 |= (*ibidata++) << I3C_IBIEXT2_EXT6_SHIFT;
			size--;
		}

		if (size > 0) {
			ext2 |= (*ibidata++) << I3C_IBIEXT2_EXT7_SHIFT;
			size--;
		}

		writeb_relaxed(ext2, svc->regs + I3C_IBIEXT2);
	}

	/* Issue IBI*/
	writel_relaxed(val, svc->regs + I3C_SCTRL);
	spin_unlock_irqrestore(&svc->ctrl_lock, flags);

	ret = readl_relaxed_poll_timeout(svc->regs + I3C_SCTRL, val,
					 !(val & I3C_SCTRL_EVENT_MASK), 0, 1000000);
	if (ret) {
		dev_err(&ctrl->dev, "Timeout when polling for IBI finish\n");

		//clear event to above hang bus
		spin_lock_irqsave(&svc->ctrl_lock, flags);
		val = readl_relaxed(svc->regs + I3C_SCTRL);
		val &= ~I3C_SCTRL_EVENT_MASK;
		writel_relaxed(val, svc->regs + I3C_SCTRL);
		spin_unlock_irqrestore(&svc->ctrl_lock, flags);

		return -ENAVAIL;
	}

	return 0;
}

static void svc_i3c_slave_complete(struct work_struct *work)
{
	struct svc_i3c_slave *svc = container_of(work, struct svc_i3c_slave, work);
	struct i3c_request *req;
	unsigned long flags;

	spin_lock_irqsave(&svc->cq_lock, flags);
	while (!list_empty(&svc->cq)) {
		req = list_first_entry(&svc->cq, struct i3c_request, list);
		list_del(&req->list);
		spin_unlock_irqrestore(&svc->cq_lock, flags);
		req->complete(req);

		spin_lock_irqsave(&svc->cq_lock, flags);
	}
	spin_unlock_irqrestore(&svc->cq_lock, flags);
}

static irqreturn_t svc_i3c_slave_irq_handler(int irq, void *dev_id)
{
	struct i3c_request *req, *complete = NULL;
	struct svc_i3c_slave *svc = dev_id;
	unsigned long flags;
	u32 statusFlags;

	statusFlags = readl(svc->regs + I3C_SSTATUS);
	writel(statusFlags, svc->regs + I3C_SSTATUS);

	if (statusFlags & I3C_SSTATUS_DACHG_MASK)
		complete_all(&svc->dacomplete);

	if (statusFlags & I3C_SSTATUS_RX_PEND_MASK) {
		spin_lock_irqsave(&svc->rxq_lock, flags);
		req = list_first_entry_or_null(&svc->rxq, struct i3c_request, list);

		if (!req) {
			writel_relaxed(I3C_SINT_RXPEND, svc->regs + I3C_SINTCLR);
		} else {
			while (!(readl_relaxed(svc->regs + I3C_SDATACTRL) &
					       I3C_SDATACTRL_RXEMPTY_MASK)) {
				*(u8 *)(req->buf + req->actual) =
							readl_relaxed(svc->regs + I3C_SRDATAB);
				req->actual++;

				if (req->actual == req->length) {
					complete = req;
					list_del(&req->list);
					break;
				}
			}

			if (req->actual != req->length && (statusFlags & I3C_SSTATUS_STOP_MASK)) {
				complete = req;
				list_del(&req->list);
			}
		}
		spin_unlock_irqrestore(&svc->rxq_lock, flags);

		if (complete) {
			spin_lock_irqsave(&svc->cq_lock, flags);
			list_add_tail(&complete->list, &svc->cq);
			spin_unlock_irqrestore(&svc->cq_lock, flags);
			queue_work(svc->workqueue, &svc->work);
			complete = NULL;
		}
	}

	if (statusFlags & I3C_SSTATUS_TXNOTFULL_MASK) {
		svc_i3c_fill_txfifo(svc);

		spin_lock_irqsave(&svc->txq_lock, flags);
		if (list_empty(&svc->txq))
			writel_relaxed(I3C_SINT_TXSEND, svc->regs + I3C_SINTCLR);
		spin_unlock_irqrestore(&svc->txq_lock, flags);
	}

	return IRQ_HANDLED;
}

static void svc_i3c_cancel_all_reqs(struct i3c_slave_ctrl *ctrl, bool tx)
{
	struct svc_i3c_slave *svc;
	struct i3c_request *req;
	struct list_head *q;
	unsigned long flags;
	spinlock_t *lk;

	svc = dev_get_drvdata(&ctrl->dev);
	if (!svc)
		return;

	if (tx) {
		q = &svc->txq;
		lk = &svc->txq_lock;
	} else {
		q = &svc->rxq;
		lk = &svc->rxq_lock;
	}

	spin_lock_irqsave(lk, flags);
	while (!list_empty(q)) {
		req = list_first_entry(q, struct i3c_request, list);
		list_del(&req->list);
		spin_unlock_irqrestore(lk, flags);

		req->status = I3C_REQUEST_CANCEL;
		req->complete(req);
		spin_lock_irqsave(lk, flags);
	}
	spin_unlock_irqrestore(lk, flags);
}

static int svc_i3c_hotjoin(struct i3c_slave_ctrl *ctrl)
{
	struct svc_i3c_slave *svc;
	int ret;
	u32 val;
	u32 cfg;

	svc = dev_get_drvdata(&ctrl->dev);
	if (!svc)
		return -EINVAL;

	reinit_completion(&svc->dacomplete);

	val = readl_relaxed(svc->regs + I3C_SSTATUS);
	if (val & I3C_SSTATUS_HJDIS_MASK) {
		dev_err(&ctrl->dev, "Hotjoin disabled by i3c master\n");
		return -EINVAL;
	}

	ret = readl_relaxed_poll_timeout(svc->regs + I3C_SCTRL, val,
					 !(val & I3C_SCTRL_EVENT_MASK), 0, 10000);
	if (ret) {
		dev_err(&ctrl->dev, "Timeout when polling for none event pending");
		return -ENAVAIL;
	}

	cfg = readl_relaxed(svc->regs + I3C_SCONFIG);
	cfg |= I3C_SCONFIG_OFFLINE_MASK;
	writel_relaxed(cfg, svc->regs + I3C_SCONFIG);

	val &= ~(I3C_SCTRL_EVENT_MASK | I3C_SCTRL_IBIDATA_MASK);
	val |= FIELD_PREP(I3C_SCTRL_EVENT_MASK, I3C_SCTRL_EVENT_HOTJOIN);
	/* Issue hotjoin*/
	writel_relaxed(val, svc->regs + I3C_SCTRL);

	ret = readl_relaxed_poll_timeout(svc->regs + I3C_SCTRL, val,
					 !(val & I3C_SCTRL_EVENT_MASK), 0, 100000);
	if (ret) {
		val &= ~FIELD_PREP(I3C_SCTRL_EVENT_MASK, I3C_SCTRL_EVENT_MASK);
		writel_relaxed(val, svc->regs + I3C_SCTRL);
		dev_err(&ctrl->dev, "Timeout when polling for HOTJOIN finish\n");
		return -EINVAL;
	}

	val = readl_relaxed(svc->regs + I3C_SSTATUS);
	val = FIELD_GET(I3C_SSTATUS_EVDET_MASK, val);
	if (val != I3C_SSTATUS_EVDET_ACKED) {
		dev_err(&ctrl->dev, "Master NACKED hotjoin request\n");
		return -EINVAL;
	}

	writel_relaxed(I3C_SINT_DACHG, svc->regs + I3C_SINTSET);
	ret = wait_for_completion_timeout(&svc->dacomplete, msecs_to_jiffies(100));
	writel_relaxed(I3C_SINT_DACHG, svc->regs + I3C_SINTCLR);
	if (!ret) {
		dev_err(&ctrl->dev, "wait for da assignment timeout\n");
		return -EIO;
	}

	val = readl_relaxed(svc->regs + I3C_SMAPCTRL0);
	val = FIELD_GET(I3C_SMAPCTRL0_DA_MASK, val);
	dev_info(&ctrl->dev, "Get dynamtic address 0x%x\n", val);
	return 0;
}

static int svc_i3c_set_status_format1(struct i3c_slave_ctrl *ctrl, u16 status)
{
	struct svc_i3c_slave *svc;
	unsigned long flags;
	u32 val;

	svc = dev_get_drvdata(&ctrl->dev);

	spin_lock_irqsave(&svc->ctrl_lock, flags);
	val = readl_relaxed(svc->regs + I3C_SCTRL);
	val &= 0xFFFF;
	val |= status << 16;
	writel_relaxed(val, svc->regs + I3C_SCTRL);
	spin_unlock_irqrestore(&svc->ctrl_lock, flags);

	return 0;
}

static u16 svc_i3c_get_status_format1(struct i3c_slave_ctrl *ctrl)
{
	struct svc_i3c_slave *svc;

	svc = dev_get_drvdata(&ctrl->dev);

	return readl_relaxed(svc->regs + I3C_SCTRL) >> 16;
}

static u8 svc_i3c_get_addr(struct i3c_slave_ctrl *ctrl)
{
	struct svc_i3c_slave *svc;
	int val;

	svc = dev_get_drvdata(&ctrl->dev);

	val = readl_relaxed(svc->regs + I3C_SMAPCTRL0);

	if (val & I3C_SMAPCTRL0_ENA_MASK)
		return FIELD_GET(I3C_SMAPCTRL0_DA_MASK, val);

	return 0;
}

int svc_i3c_fifo_status(struct i3c_slave_ctrl *ctrl, bool tx)
{
	struct svc_i3c_slave *svc;
	int val;

	svc = dev_get_drvdata(&ctrl->dev);

	val = readl_relaxed(svc->regs + I3C_SDATACTRL);

	if (tx)
		return FIELD_GET(I3C_SDATACTRL_TXCOUNT_MASK, val);
	else
		return FIELD_GET(I3C_SDATACTRL_RXCOUNT_MASK, val);
}

static struct i3c_slave_ctrl_ops svc_i3c_slave_ops = {
	.set_config = svc_i3c_slave_set_config,
	.enable = svc_i3c_slave_enable,
	.disable = svc_i3c_slave_disable,
	.queue = svc_i3c_slave_queue,
	.dequeue = svc_i3c_dequeue,
	.raise_ibi = svc_i3c_slave_raise_ibi,
	.fifo_flush = svc_i3c_slave_fifo_flush,
	.cancel_all_reqs = svc_i3c_cancel_all_reqs,
	.get_features = svc_i3c_get_features,
	.hotjoin = svc_i3c_hotjoin,
	.fifo_status = svc_i3c_fifo_status,
	.set_status_format1 = svc_i3c_set_status_format1,
	.get_status_format1 = svc_i3c_get_status_format1,
	.get_addr = svc_i3c_get_addr,
};

static int svc_i3c_slave_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i3c_slave_ctrl *slave;
	struct svc_i3c_slave *svc;
	int ret;
	u32 val;

	svc = devm_kzalloc(dev, sizeof(*svc), GFP_KERNEL);
	if (!svc)
		return -ENOMEM;

	svc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(svc->regs))
		return PTR_ERR(svc->regs);

	svc->clks[PCLK].id = "pclk";
	svc->clks[FCLK].id = "fast_clk";
	svc->clks[SCLK].id = "slow_clk";

	ret = devm_clk_bulk_get(dev, MAXCLK, svc->clks);
	if (ret < 0) {
		dev_err(dev, "fail get clks: %d\n", ret);
		return ret;
	}

	ret = clk_bulk_prepare_enable(MAXCLK, svc->clks);
	if (ret < 0) {
		dev_err(dev, "fail enable clks: %d\n", ret);
		return ret;
	}

	svc->irq = platform_get_irq(pdev, 0);
	if (svc->irq < 0)
		return svc->irq;

	INIT_LIST_HEAD(&svc->txq);
	INIT_LIST_HEAD(&svc->rxq);
	INIT_LIST_HEAD(&svc->cq);
	spin_lock_init(&svc->txq_lock);
	spin_lock_init(&svc->rxq_lock);
	spin_lock_init(&svc->cq_lock);
	spin_lock_init(&svc->ctrl_lock);

	init_completion(&svc->dacomplete);

	INIT_WORK(&svc->work, svc_i3c_slave_complete);
	svc->workqueue = alloc_workqueue("%s-cq", 0, 0, dev_name(dev));
	if (!svc->workqueue)
		return -ENOMEM;

	/* Disable all IRQ */
	writel_relaxed(0xFFFFFFFF, svc->regs + I3C_SINTCLR);

	val = readl_relaxed(svc->regs + I3C_SCAPABILITIES);
	svc->features.tx_fifo_sz  = FIELD_GET(I3C_SCAPABILITIES_FIFOTX_MASK, val);
	svc->features.tx_fifo_sz = 2 << svc->features.tx_fifo_sz;

	svc->features.rx_fifo_sz = FIELD_GET(I3C_SCAPABILITIES_FIFORX_MASK, val);
	svc->features.rx_fifo_sz = 2 << svc->features.rx_fifo_sz;

	ret = devm_request_irq(dev, svc->irq, svc_i3c_slave_irq_handler, 0, "svc-i3c-irq", svc);
	if (ret)
		return -ENOENT;

	slave = devm_i3c_slave_ctrl_create(dev, &svc_i3c_slave_ops);
	if (!slave)
		return -ENOMEM;

	dev_set_drvdata(&slave->dev, svc);

	return 0;
}

static int svc_i3c_slave_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id svc_i3c_slave_of_match_tbl[] = {
	{ .compatible = "silvaco,i3c-slave-v1" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, svc_i3c_slave_of_match_tbl);

static struct platform_driver svc_i3c_slave = {
	.probe = svc_i3c_slave_probe,
	.remove = svc_i3c_slave_remove,
	.driver = {
		.name = "silvaco-i3c-slave",
		.of_match_table = svc_i3c_slave_of_match_tbl,
		//.pm = &svc_i3c_pm_ops,
	},
};
module_platform_driver(svc_i3c_slave);

MODULE_DESCRIPTION("Silvaco dual-role I3C slave driver");
MODULE_LICENSE("GPL");
