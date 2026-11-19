// SPDX-License-Identifier: GPL-2.0
/*
 * AMD MicroBlaze/V BRAM-based Remote Processor driver
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * This driver supports soft-core processors (MicroBlaze, MicroBlaze-V, or
 * similar) instantiated in AMD programmable logic, using dual-port BRAM
 * for firmware storage and execution.
 *
 * The firmware memory (BRAM) is described in the processor-local address
 * space and translated to the Linux-visible system physical address with
 * standard devicetree address translation.
 *
 * Reset is controlled via GPIO connected to Processor System Reset IP.
 */

#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/mailbox/zynqmp-ipi-message.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/workqueue.h>

#include "remoteproc_internal.h"

/**
 * struct amd_bram_rproc - AMD MicroBlaze/V BRAM-based remoteproc private data
 * @dev: device pointer
 * @reset: GPIO descriptor for reset control (active-low)
 * @clk: processor clock
 * @rproc: remote processor instance
 * @tx_client: mailbox client used for virtqueue kicks
 * @rx_client: mailbox client used for remote notifications
 * @tx_chan: transmit mailbox channel
 * @rx_chan: receive mailbox channel
 * @notify_msgs: persistent mailbox payload storage indexed by notify ID
 * @pending_notify_ids: notifications awaiting process-context dispatch
 * @rx_work: work item that dispatches received virtqueue notifications
 */
struct amd_bram_rproc {
	struct device *dev;
	struct gpio_desc *reset;
	struct clk *clk;
	struct rproc *rproc;
	struct mbox_client tx_client;
	struct mbox_client rx_client;
	struct mbox_chan *tx_chan;
	struct mbox_chan *rx_chan;
	struct zynqmp_ipi_message *notify_msgs[256];
	DECLARE_BITMAP(pending_notify_ids, 256);
	struct work_struct rx_work;
};

static void amd_bram_rproc_rx_work(struct work_struct *work)
{
	struct amd_bram_rproc *priv =
		container_of(work, struct amd_bram_rproc, rx_work);
	unsigned long notifyid;

	for_each_set_bit(notifyid, priv->pending_notify_ids,
			 ARRAY_SIZE(priv->notify_msgs)) {
		clear_bit(notifyid, priv->pending_notify_ids);
		if (rproc_vq_interrupt(priv->rproc, notifyid) == IRQ_NONE)
			dev_dbg(priv->dev, "no virtqueue for notifyid %lu\n",
				notifyid);
	}
}

static void amd_bram_rproc_rx_callback(struct mbox_client *client, void *msg)
{
	struct amd_bram_rproc *priv =
		container_of(client, struct amd_bram_rproc, rx_client);
	struct zynqmp_ipi_message *mb_msg = msg;
	size_t offset;
	u32 notifyid;

	if (!msg) {
		dev_warn_ratelimited(priv->dev,
				     "received mailbox notification without data\n");
		return;
	}

	/* Each notification is one 32-bit notify ID. */
	if (!mb_msg->len || mb_msg->len % sizeof(u32)) {
		dev_warn_ratelimited(priv->dev,
				     "malformed mailbox notification: %zu bytes\n",
				     mb_msg->len);
		return;
	}

	for (offset = 0; offset < mb_msg->len; offset += sizeof(u32)) {
		memcpy(&notifyid, &mb_msg->data[offset], sizeof(notifyid));
		if (notifyid >= ARRAY_SIZE(priv->notify_msgs)) {
			dev_warn_ratelimited(priv->dev, "invalid notify id %u\n",
					     notifyid);
			continue;
		}

		set_bit(notifyid, priv->pending_notify_ids);
	}

	schedule_work(&priv->rx_work);
}

static void amd_bram_rproc_kick(struct rproc *rproc, int vqid)
{
	struct amd_bram_rproc *priv = rproc->priv;
	struct zynqmp_ipi_message *msg;
	int ret;

	if (!priv->tx_chan) {
		dev_warn_ratelimited(priv->dev,
				     "cannot kick vq%d: no TX mailbox\n", vqid);
		return;
	}

	if (vqid < 0 || vqid >= ARRAY_SIZE(priv->notify_msgs)) {
		dev_err_ratelimited(priv->dev, "invalid notify id %d\n", vqid);
		return;
	}

	/*
	 * The mailbox core queues message pointers. Keep each notification word
	 * in persistent storage until the controller reports TX completion.
	 */
	msg = priv->notify_msgs[vqid];
	msg->len = sizeof(u32);
	memcpy(msg->data, &vqid, sizeof(u32));
	/* Order writes to WC vring mappings before notifying the remote. */
	wmb();
	ret = mbox_send_message(priv->tx_chan, msg);
	if (ret < 0)
		dev_err_ratelimited(priv->dev, "failed to kick vq%d: %d\n",
				    vqid, ret);
}

static void amd_bram_rproc_free_mboxes(struct amd_bram_rproc *priv)
{
	if (priv->rx_chan) {
		mbox_free_channel(priv->rx_chan);
		priv->rx_chan = NULL;
	}
	if (priv->tx_chan) {
		mbox_free_channel(priv->tx_chan);
		priv->tx_chan = NULL;
	}
}

static int amd_bram_rproc_request_mboxes(struct amd_bram_rproc *priv)
{
	int ret;

	/* Without mailboxes the driver supports load, start, and stop only. */
	if (!of_property_present(priv->dev->of_node, "mboxes"))
		return 0;

	priv->tx_chan = mbox_request_channel_byname(&priv->tx_client, "tx");
	if (IS_ERR(priv->tx_chan)) {
		ret = PTR_ERR(priv->tx_chan);
		priv->tx_chan = NULL;
		return dev_err_probe(priv->dev, ret,
				     "failed to request tx mailbox\n");
	}

	priv->rx_chan = mbox_request_channel_byname(&priv->rx_client, "rx");
	if (IS_ERR(priv->rx_chan)) {
		ret = PTR_ERR(priv->rx_chan);
		priv->rx_chan = NULL;
		mbox_free_channel(priv->tx_chan);
		priv->tx_chan = NULL;
		return dev_err_probe(priv->dev, ret,
				     "failed to request rx mailbox\n");
	}

	return 0;
}

static void amd_bram_rproc_release_mboxes(void *data)
{
	struct amd_bram_rproc *priv = data;

	/*
	 * Free the channels before cancelling the receive work. RX channel
	 * shutdown disables the controller interrupt synchronously, so no
	 * notification can queue the work again once it has been cancelled.
	 */
	amd_bram_rproc_free_mboxes(priv);
	cancel_work_sync(&priv->rx_work);
}

static int amd_bram_rproc_reserved_mem_count(struct amd_bram_rproc *priv)
{
	struct device_node *np = priv->dev->of_node;

	if (!of_property_present(np, "memory-region"))
		return 0;

	return of_reserved_mem_region_count(np);
}

static int amd_bram_rproc_get_reserved_mem(struct amd_bram_rproc *priv, int i,
					   const char **name,
					   struct resource *res)
{
	struct device_node *np = priv->dev->of_node;
	int ret;

	ret = of_property_read_string_index(np, "memory-region-names", i, name);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to read memory-region name %d\n", i);

	ret = of_reserved_mem_region_to_resource(np, i, res);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to parse memory-region %d\n", i);

	/* The remote uses the physical address as device address. */
	if (res->end > U32_MAX)
		return dev_err_probe(priv->dev, -EINVAL,
				     "memory-region %d %pR is above 4 GiB\n",
				     i, res);

	return 0;
}

/* Validate all regions so that nothing is registered if one is invalid. */
static int amd_bram_rproc_check_reserved_mem(struct amd_bram_rproc *priv)
{
	struct resource res;
	const char *name;
	int count;
	int ret;
	int i;

	count = amd_bram_rproc_reserved_mem_count(priv);
	if (count < 0)
		return count;

	for (i = 0; i < count; i++) {
		ret = amd_bram_rproc_get_reserved_mem(priv, i, &name, &res);
		if (ret)
			return ret;
	}

	return 0;
}

static int amd_bram_rproc_add_reserved_mem(struct rproc *rproc)
{
	struct amd_bram_rproc *priv = rproc->priv;
	struct rproc_mem_entry *mem;
	int count;
	int ret;
	int i;

	count = amd_bram_rproc_reserved_mem_count(priv);
	if (count < 0)
		return count;

	for (i = 0; i < count; i++) {
		struct resource res;
		const char *name;

		ret = amd_bram_rproc_get_reserved_mem(priv, i, &name, &res);
		if (ret)
			return ret;

		if (!strcmp(name, "vdev0buffer"))
			mem = rproc_of_resm_mem_entry_init(priv->dev, i,
							   resource_size(&res),
							   res.start, name);
		else
			mem = rproc_mem_entry_init(priv->dev, NULL,
						   (dma_addr_t)res.start,
						   resource_size(&res), res.start,
						   rproc_mem_entry_ioremap_wc,
						   rproc_mem_entry_iounmap,
						   "%s", name);
		if (!mem)
			return -ENOMEM;

		rproc_add_carveout(rproc, mem);
		rproc_coredump_add_segment(rproc, res.start, resource_size(&res));
		dev_dbg(priv->dev, "reserved memory carveout %pR\n", &res);
	}

	return 0;
}

static int amd_bram_rproc_prepare(struct rproc *rproc)
{
	struct amd_bram_rproc *priv = rproc->priv;
	struct rproc_mem_entry *mem;
	struct resource res;
	u64 da, size;
	int ret;

	ret = of_property_read_reg(priv->dev->of_node, 0, &da, &size);
	if (ret) {
		dev_err(priv->dev, "failed to parse executable memory reg\n");
		return ret;
	}

	if (!size || size > U32_MAX) {
		dev_err(priv->dev, "invalid executable memory size\n");
		return -EINVAL;
	}

	if (da > U32_MAX) {
		dev_err(priv->dev, "invalid executable memory address\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(priv->dev->of_node, 0, &res);
	if (ret) {
		dev_err(priv->dev, "failed to translate executable memory reg\n");
		return ret;
	}

	ret = amd_bram_rproc_check_reserved_mem(priv);
	if (ret)
		return ret;

	mem = rproc_mem_entry_init(priv->dev, NULL, (dma_addr_t)res.start,
				   resource_size(&res), da,
				   rproc_mem_entry_ioremap_wc,
				   rproc_mem_entry_iounmap,
				   dev_name(priv->dev));
	if (!mem)
		return -ENOMEM;

	rproc_add_carveout(rproc, mem);
	rproc_coredump_add_segment(rproc, da, resource_size(&res));

	return amd_bram_rproc_add_reserved_mem(rproc);
}

static int amd_bram_rproc_start(struct rproc *rproc)
{
	struct amd_bram_rproc *priv = rproc->priv;
	int ret;

	/* Enable clock before releasing reset */
	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(priv->dev, "failed to enable clock: %d\n", ret);
		return ret;
	}

	/* Deassert reset and let the processor run. */
	ret = gpiod_set_value_cansleep(priv->reset, 0);
	if (ret) {
		dev_err(priv->dev, "failed to deassert reset: %d\n", ret);
		clk_disable_unprepare(priv->clk);
		return ret;
	}

	return 0;
}

static int amd_bram_rproc_stop(struct rproc *rproc)
{
	struct amd_bram_rproc *priv = rproc->priv;
	int ret;

	/* Assert reset before disabling the processor clock. */
	ret = gpiod_set_value_cansleep(priv->reset, 1);
	if (ret) {
		dev_err(priv->dev, "failed to assert reset: %d\n", ret);
		return ret;
	}
	cancel_work_sync(&priv->rx_work);
	bitmap_zero(priv->pending_notify_ids,
		    ARRAY_SIZE(priv->notify_msgs));

	/* Disable clock after asserting reset */
	clk_disable_unprepare(priv->clk);

	return 0;
}

static int amd_bram_rproc_parse_fw(struct rproc *rproc,
				   const struct firmware *fw)
{
	rproc_elf_load_rsc_table_optional(rproc, fw, dev_dbg,
					  "no resource table found\n");
	return 0;
}

static const struct rproc_ops amd_bram_rproc_ops = {
	.prepare	= amd_bram_rproc_prepare,
	.start		= amd_bram_rproc_start,
	.stop		= amd_bram_rproc_stop,
	.load		= rproc_elf_load_segments,
	.sanity_check	= rproc_elf_sanity_check,
	.get_boot_addr	= rproc_elf_get_boot_addr,
	.parse_fw	= amd_bram_rproc_parse_fw,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.kick		= amd_bram_rproc_kick,
};

static int amd_bram_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct amd_bram_rproc *priv;
	const char *fw_name = NULL;
	struct rproc *rproc;
	int ret;
	int i;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret < 0 && ret != -EINVAL)
		return dev_err_probe(dev, ret,
				     "failed to parse firmware-name property\n");

	rproc = devm_rproc_alloc(dev, dev_name(dev), &amd_bram_rproc_ops,
				 fw_name, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	priv = rproc->priv;
	priv->dev = dev;
	priv->rproc = rproc;
	INIT_WORK(&priv->rx_work, amd_bram_rproc_rx_work);

	/* Get the processor clock */
	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to get clock\n");

	/*
	 * Keep the processor in reset until remoteproc has finished loading
	 * firmware into the executable memory window described by reg and
	 * translated through the parent bus ranges property.
	 */
	priv->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "failed to get reset gpio\n");

	priv->tx_client.dev = dev;
	priv->tx_client.tx_block = false;
	priv->tx_client.knows_txdone = false;
	priv->rx_client.dev = dev;
	priv->rx_client.rx_callback = amd_bram_rproc_rx_callback;
	for (i = 0; i < ARRAY_SIZE(priv->notify_msgs); i++) {
		priv->notify_msgs[i] =
			devm_kzalloc(dev, struct_size(priv->notify_msgs[i],
						      data, sizeof(u32)),
				     GFP_KERNEL);
		if (!priv->notify_msgs[i])
			return -ENOMEM;
	}

	ret = amd_bram_rproc_request_mboxes(priv);
	if (ret)
		return ret;

	/* Registered before the rproc, so it runs after the rproc is stopped. */
	ret = devm_add_action_or_reset(dev, amd_bram_rproc_release_mboxes, priv);
	if (ret)
		return ret;

	rproc->auto_boot = false;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	platform_set_drvdata(pdev, rproc);

	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register rproc\n");

	return 0;
}

static const struct of_device_id amd_bram_rproc_of_match[] = {
	{ .compatible = "xlnx,zynqmp-bram-rproc" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, amd_bram_rproc_of_match);

static struct platform_driver amd_bram_rproc_driver = {
	.probe = amd_bram_rproc_probe,
	.driver = {
		.name = "amd-bram-rproc",
		.of_match_table = amd_bram_rproc_of_match,
	},
};
module_platform_driver(amd_bram_rproc_driver);

MODULE_DESCRIPTION("AMD MicroBlaze/V BRAM-based Remote Processor driver");
MODULE_AUTHOR("Ben Levinsky <ben.levinsky@amd.com>");
MODULE_LICENSE("GPL");
