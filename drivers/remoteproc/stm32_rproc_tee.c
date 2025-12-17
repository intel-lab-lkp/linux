// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025, STMicroelectronics - All Rights Reserved
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc_tee.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "remoteproc_internal.h"

#define MBOX_NB_VQ		2
#define MBOX_NB_MBX		3

#define STM32_MBX_VQ0		"vq0"
#define STM32_MBX_VQ0_ID	0
#define STM32_MBX_VQ1		"vq1"
#define STM32_MBX_VQ1_ID	1
#define STM32_MBX_SHUTDOWN	"shutdown"

struct stm32_rproc_tee_mem {
	char name[20];
	void __iomem *cpu_addr;
	phys_addr_t phy_addr;
	u32 dev_addr;
	size_t size;
};

struct stm32_rproc_tee_dma_ranges {
	u32 dev_addr;
	u32 phy_addr;
	u32 size;
};

struct stm32_mbox {
	const unsigned char name[10];
	struct mbox_chan *chan;
	struct mbox_client client;
	struct work_struct vq_work;
	int vq_id;
};

struct stm32_rproc_tee {
	int wdg_irq;
	const struct stm32_rproc_tee_dma_ranges *ranges;
	struct stm32_mbox mb[MBOX_NB_MBX];
	struct workqueue_struct *workqueue;
};

static const struct stm32_rproc_tee_dma_ranges stm32mp15_m4_dma_ranges[] = {
	/* RETRAM address translation */
	{ .dev_addr = 0x0, .phy_addr = 0x38000000, .size = 0x10000 },
	{/* sentinel */}
};

static int stm32_rproc_tee_pa_to_da(struct rproc *rproc, phys_addr_t pa,
				    size_t size, u64 *da)
{
	struct stm32_rproc_tee *ddata = rproc->priv;
	const struct stm32_rproc_tee_dma_ranges *range;
	struct device *dev = rproc->dev.parent;

	for (range = ddata->ranges; range->phy_addr; range++) {
		if (pa < range->phy_addr ||
		    pa >= range->phy_addr + range->size)
			continue;
		if (pa + size > range->phy_addr + range->size) {
			dev_err(dev, "range too small for %pa+0x%zx\n", &pa, size);
			return -EINVAL;
		}
		*da = pa - range->phy_addr + range->dev_addr;
		dev_dbg(dev, "pa %pa to da %llx\n", &pa, *da);
		return 0;
	}

	*da = pa;

	return 0;
}

static int stm32_rproc_tee_mem_alloc(struct rproc *rproc,
				     struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	dev_dbg(dev, "map memory: %pad+%zx\n", &mem->dma, mem->len);
	va = (__force void *)ioremap_wc(mem->dma, mem->len);
	if (IS_ERR_OR_NULL(va)) {
		dev_err(dev, "Unable to map memory region: %pad+0x%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;

	return 0;
}

static int stm32_rproc_tee_mem_release(struct rproc *rproc,
				       struct rproc_mem_entry *mem)
{
	dev_dbg(rproc->dev.parent, "unmap memory: %pa\n", &mem->dma);
	iounmap((__force __iomem void *)mem->va);

	return 0;
}

static int stm32_rproc_tee_mbox_idx(struct rproc *rproc, const unsigned char *name)
{
	struct stm32_rproc_tee *ddata = rproc->priv;
	int i;

	for (i = 0; i < ARRAY_SIZE(ddata->mb); i++) {
		if (!strncmp(ddata->mb[i].name, name, strlen(name)))
			return i;
	}
	dev_err(&rproc->dev, "mailbox %s not found\n", name);

	return -EINVAL;
}

static void stm32_rproc_tee_request_shutdown(struct rproc *rproc)
{
	struct stm32_rproc_tee *ddata = rproc->priv;
	int err, idx;

	/* Request shutdown of the remote processor */
	if (rproc->state != RPROC_OFFLINE && rproc->state != RPROC_CRASHED) {
		idx = stm32_rproc_tee_mbox_idx(rproc, STM32_MBX_SHUTDOWN);
		if (idx >= 0 && ddata->mb[idx].chan) {
			err = mbox_send_message(ddata->mb[idx].chan, "detach");
			if (err < 0)
				dev_warn(&rproc->dev, "warning: remote FW shutdown without ack\n");
		}
	}
}

static int stm32_rproc_tee_stop(struct rproc *rproc)
{
	stm32_rproc_tee_request_shutdown(rproc);

	return rproc_tee_stop(rproc);
}

static int stm32_rproc_tee_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct rproc_mem_entry *mem;
	int index = 0, mr = 0;
	u64 da;

	/* Register associated reserved memory regions */
	while (1) {
		struct resource res;
		int ret;

		ret = of_reserved_mem_region_to_resource(np, mr++, &res);
		if (ret)
			return 0;

		if (stm32_rproc_tee_pa_to_da(rproc, res.start,
					     resource_size(&res), &da) < 0) {
			dev_err(dev, "memory region not valid %pR\n", &res);
			return -EINVAL;
		}

		/* No need to map vdev buffer */
		if (strstarts(res.name, "vdev0buffer")) {
			/* Register reserved memory for vdev buffer alloc */
			mem = rproc_of_resm_mem_entry_init(dev, index,
							   resource_size(&res),
							   res.start,
							   "vdev0buffer");
		} else {
			/* Register memory region */
			mem = rproc_mem_entry_init(dev, NULL,
						   (dma_addr_t)res.start,
						   resource_size(&res), da,
						   stm32_rproc_tee_mem_alloc,
						   stm32_rproc_tee_mem_release,
						   "%.*s",
						   (int)(strchrnul(res.name, '@') - res.name),
						   res.name);
			if (mem)
				rproc_coredump_add_segment(rproc, da,
							   resource_size(&res));
		}

		if (!mem)
			return -ENOMEM;

		rproc_add_carveout(rproc, mem);
		index++;
	}

	return 0;
}

static irqreturn_t stm32_rproc_tee_wdg(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_report_crash(rproc, RPROC_WATCHDOG);

	return IRQ_HANDLED;
}

static void stm32_rproc_tee_mb_vq_work(struct work_struct *work)
{
	struct stm32_mbox *mb = container_of(work, struct stm32_mbox, vq_work);
	struct rproc *rproc = dev_get_drvdata(mb->client.dev);

	mutex_lock(&rproc->lock);

	if (rproc->state != RPROC_RUNNING && rproc->state != RPROC_ATTACHED)
		goto unlock_mutex;

	if (rproc_vq_interrupt(rproc, mb->vq_id) == IRQ_NONE)
		dev_dbg(&rproc->dev, "no message found in vq%d\n", mb->vq_id);

unlock_mutex:
	mutex_unlock(&rproc->lock);
}

static void stm32_rproc_tee_mb_callback(struct mbox_client *cl, void *data)
{
	struct rproc *rproc = dev_get_drvdata(cl->dev);
	struct stm32_mbox *mb = container_of(cl, struct stm32_mbox, client);
	struct stm32_rproc_tee *ddata = rproc->priv;

	queue_work(ddata->workqueue, &mb->vq_work);
}

static void stm32_rproc_tee_free_mbox(struct rproc *rproc)
{
	struct stm32_rproc_tee *ddata = rproc->priv;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ddata->mb); i++) {
		if (ddata->mb[i].chan)
			mbox_free_channel(ddata->mb[i].chan);
		ddata->mb[i].chan = NULL;
	}
}

static const struct stm32_mbox stm32_rproc_tee_mbox[MBOX_NB_MBX] = {
	{
		.name = STM32_MBX_VQ0,
		.vq_id = STM32_MBX_VQ0_ID,
		.client = {
			.rx_callback = stm32_rproc_tee_mb_callback,
			.tx_block = false,
		},
	},
	{
		.name = STM32_MBX_VQ1,
		.vq_id = STM32_MBX_VQ1_ID,
		.client = {
			.rx_callback = stm32_rproc_tee_mb_callback,
			.tx_block = false,
		},
	},
	{
		.name = STM32_MBX_SHUTDOWN,
		.vq_id = -1,
		.client = {
			.tx_block = true,
			.tx_done = NULL,
			.tx_tout = 500, /* 500 ms time out */
		},
	},
};

static int stm32_rproc_tee_request_mbox(struct rproc *rproc)
{
	struct stm32_rproc_tee *ddata = rproc->priv;
	struct device *dev = &rproc->dev;
	unsigned int i;
	int j;
	const unsigned char *name;
	struct mbox_client *cl;

	/* Initialise mailbox structure table */
	memcpy(ddata->mb, stm32_rproc_tee_mbox, sizeof(stm32_rproc_tee_mbox));

	for (i = 0; i < MBOX_NB_MBX; i++) {
		name = ddata->mb[i].name;

		cl = &ddata->mb[i].client;
		cl->dev = dev->parent;

		ddata->mb[i].chan = mbox_request_channel_byname(cl, name);
		if (IS_ERR(ddata->mb[i].chan)) {
			if (PTR_ERR(ddata->mb[i].chan) == -EPROBE_DEFER) {
				dev_err_probe(dev->parent,
					      PTR_ERR(ddata->mb[i].chan),
					      "failed to request mailbox %s\n",
					      name);
				goto err_probe;
			}
			dev_info(dev, "mailbox %s mbox not used\n", name);
			ddata->mb[i].chan = NULL;
		}
		if (ddata->mb[i].vq_id >= 0) {
			INIT_WORK(&ddata->mb[i].vq_work,
				  stm32_rproc_tee_mb_vq_work);
		}
	}

	return 0;

err_probe:
	for (j = i - 1; j >= 0; j--)
		if (ddata->mb[j].chan) {
			mbox_free_channel(ddata->mb[j].chan);
			ddata->mb[j].chan = NULL;
		}
	return -EPROBE_DEFER;
}

static void stm32_rproc_tee_kick(struct rproc *rproc, int vqid)
{
	struct stm32_rproc_tee *ddata = rproc->priv;
	unsigned int i;
	int err;

	if (WARN_ON(vqid >= MBOX_NB_VQ))
		return;

	for (i = 0; i < MBOX_NB_MBX; i++) {
		if (vqid != ddata->mb[i].vq_id)
			continue;
		if (!ddata->mb[i].chan)
			return;
		err = mbox_send_message(ddata->mb[i].chan, "kick");
		if (err < 0)
			dev_err(&rproc->dev, "%s: failed (%s, err:%d)\n",
				__func__, ddata->mb[i].name, err);
		return;
	}
}

static const struct of_device_id stm32_rproc_tee_match[] = {
	{
		.compatible = "st,stm32mp15-m4-tee",
		.data = &stm32mp15_m4_dma_ranges,
	},
	{},
};
MODULE_DEVICE_TABLE(of, stm32_rproc_tee_match);

static int stm32_rproc_tee_parse_dt(struct platform_device *pdev,
				    struct stm32_rproc_tee *ddata,
				    bool *auto_boot)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int err, irq;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq == -EPROBE_DEFER)
		return irq;

	if (irq > 0) {
		err = devm_request_irq(dev, irq, stm32_rproc_tee_wdg, 0,
				       dev_name(dev), pdev);
		if (err)
			return dev_err_probe(dev, err,
					     "failed to request wdg irq\n");

		ddata->wdg_irq = irq;

		if (of_property_read_bool(np, "wakeup-source")) {
			device_init_wakeup(dev, true);
			dev_pm_set_wake_irq(dev, irq);
		}

		dev_info(dev, "wdg irq registered\n");
	}

	*auto_boot = of_property_read_bool(np, "st,auto-boot");

	return 0;
}

static int stm32_rproc_tee_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct stm32_rproc_tee *ddata;
	struct rproc *rproc;
	bool auto_boot;
	u32 proc_id;
	int ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "reg", &proc_id);
	if (ret) {
		dev_err(dev, "invalid reg on %pOF\n", np);
		return ret;
	}

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ret = stm32_rproc_tee_parse_dt(pdev, ddata, &auto_boot);
	if (ret)
		return ret;

	ret = rproc_tee_register(dev, &rproc, proc_id, auto_boot);
	if (ret)
		return dev_err_probe(dev, ret, "signed firmware not supported by TEE\n");

	rproc->ops->prepare = stm32_rproc_tee_prepare;
	rproc->ops->stop = stm32_rproc_tee_stop;
	rproc->ops->kick = stm32_rproc_tee_kick;

	ddata = rproc->priv;

	ddata->ranges = device_get_match_data(&pdev->dev);

	rproc->has_iommu = false;
	ddata->workqueue = create_workqueue(dev_name(dev));
	if (!ddata->workqueue) {
		dev_err(dev, "cannot create workqueue\n");
		ret = -ENOMEM;
		goto free_resources;
	}

	platform_set_drvdata(pdev, rproc);

	ret = stm32_rproc_tee_request_mbox(rproc);
	if (ret)
		goto free_wkq;

	return 0;

free_wkq:
	destroy_workqueue(ddata->workqueue);
free_resources:
	rproc_resource_cleanup(rproc);
	if (device_may_wakeup(dev)) {
		dev_pm_clear_wake_irq(dev);
		device_init_wakeup(dev, false);
	}
	rproc_tee_unregister(dev, rproc);

	return ret;
}

static void stm32_rproc_tee_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct stm32_rproc_tee *ddata = rproc->priv;
	struct device *dev = &pdev->dev;

	stm32_rproc_tee_free_mbox(rproc);
	destroy_workqueue(ddata->workqueue);

	if (device_may_wakeup(dev)) {
		dev_pm_clear_wake_irq(dev);
		device_init_wakeup(dev, false);
	}

	rproc_tee_unregister(dev, rproc);
}

static int stm32_rproc_tee_suspend(struct device *dev)
{
	struct rproc *rproc = dev_get_drvdata(dev);
	struct stm32_rproc_tee *ddata = rproc->priv;

	if (device_may_wakeup(dev))
		return enable_irq_wake(ddata->wdg_irq);

	return 0;
}

static int stm32_rproc_tee_resume(struct device *dev)
{
	struct rproc *rproc = dev_get_drvdata(dev);
	struct stm32_rproc_tee *ddata = rproc->priv;

	if (device_may_wakeup(dev))
		return disable_irq_wake(ddata->wdg_irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(stm32_rproc_tee_pm_ops,
				stm32_rproc_tee_suspend, stm32_rproc_tee_resume);

static struct platform_driver stm32_rproc_tee_driver = {
	.probe = stm32_rproc_tee_probe,
	.remove = stm32_rproc_tee_remove,
	.driver = {
		.name = "stm32-rproc-tee",
		.pm = pm_ptr(&stm32_rproc_tee_pm_ops),
		.of_match_table = stm32_rproc_tee_match,
	},
};
module_platform_driver(stm32_rproc_tee_driver);

MODULE_DESCRIPTION("STM32 Remote Processor TEE Control Driver");
MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@foss.st.com>");
MODULE_LICENSE("GPL");

