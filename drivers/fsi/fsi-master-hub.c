// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSI hub master driver
 *
 * Copyright (C) IBM Corporation 2016
 */

#include <linux/delay.h>
#include <linux/fsi.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "fsi-master.h"
#include "fsi-slave.h"

#define FSI_ENGID_HUB_MASTER		0x1c

/*
 * FSI hub master support
 *
 * A hub master increases the number of potential target devices that the
 * primary FSI master can access. For each link a primary master supports,
 * each of those links can in turn be chained to a hub master with multiple
 * links of its own.
 *
 * The hub is controlled by a set of control registers exposed as a regular fsi
 * device (the hub->upstream device), and provides access to the downstream FSI
 * bus as through an address range on the slave itself (->addr and ->size).
 *
 * [This differs from "cascaded" masters, which expose the entire downstream
 * bus entirely through the fsi device address range, and so have a smaller
 * accessible address space.]
 */
struct fsi_master_hub {
	struct fsi_master	master;
	struct irq_domain	*irq_domain;
	struct fsi_device	*upstream;
	uint32_t		addr;
	uint32_t		size;
};

#define to_fsi_master_hub(m) container_of(m, struct fsi_master_hub, master)

static int hub_master_read(struct fsi_master *master, int link,
			uint8_t id, uint32_t addr, void *val, size_t size)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);

	if (id != 0)
		return -EINVAL;

	addr += hub->addr + (link * FSI_HUB_LINK_SIZE);
	return fsi_slave_read(hub->upstream->slave, addr, val, size);
}

static int hub_master_write(struct fsi_master *master, int link,
			uint8_t id, uint32_t addr, const void *val, size_t size)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);

	if (id != 0)
		return -EINVAL;

	addr += hub->addr + (link * FSI_HUB_LINK_SIZE);
	return fsi_slave_write(hub->upstream->slave, addr, val, size);
}

static int hub_master_break(struct fsi_master *master, int link)
{
	uint32_t addr;
	__be32 cmd;

	addr = 0x4;
	cmd = cpu_to_be32(0xc0de0000);

	return hub_master_write(master, link, 0, addr, &cmd, sizeof(cmd));
}

static int hub_master_link_enable(struct fsi_master *master, int link,
				  bool enable)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);
	u32 srsim = 0xff000000 >> (8 * (link % 4));
	int slave_idx = 4 * (link / 4);
	__be32 srsim_be;
	int ret;

	ret = fsi_slave_read(hub->upstream->slave, FSI_SLAVE_BASE + FSI_SRSIM0 + slave_idx,
			     &srsim_be, sizeof(srsim_be));
	if (ret)
		return ret;

	if (enable) {
		ret = fsi_master_link_enable(master, link, enable);
		if (ret)
			return ret;

		srsim |= be32_to_cpu(srsim_be);
		srsim_be = cpu_to_be32(srsim);
		ret = fsi_slave_write(hub->upstream->slave,
				      FSI_SLAVE_BASE + FSI_SRSIM0 + slave_idx, &srsim_be,
				      sizeof(srsim_be));
	} else {
		srsim = be32_to_cpu(srsim_be) & ~srsim;
		srsim_be = cpu_to_be32(srsim);
		ret = fsi_slave_write(hub->upstream->slave,
				      FSI_SLAVE_BASE + FSI_SRSIM0 + slave_idx, &srsim_be,
				      sizeof(srsim_be));
		if (ret)
			return ret;

		ret = fsi_master_link_enable(master, link, enable);
	}

	return ret;
}

static irqreturn_t hub_master_irq(int irq, void *data)
{
	struct fsi_master_hub *hub = data;
	struct fsi_master *parent = hub->upstream->slave->master;
	unsigned int link = 0;

	for (; link < FSI_HUB_MASTER_MAX_LINKS; ++link) {
		if (parent->remote_interrupt_status & (1 << link))
			fsi_master_irq(&hub->master, hub->irq_domain, link);
	}

	return IRQ_HANDLED;
}

static int hub_master_irqd_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	struct fsi_master_hub *hub = domain->host_data;

	irq_set_chip_and_handler(irq, &hub->master.irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, &hub->master);

	return 0;
}

static const struct irq_domain_ops hub_master_irq_domain_ops = {
	.map = hub_master_irqd_map,
};

static void hub_master_release(struct device *dev)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(to_fsi_master(dev));

	if (hub->irq_domain)
		irq_domain_remove(hub->irq_domain);

	regmap_exit(hub->master.map);
	kfree(hub);
}

static int hub_master_probe(struct device *dev)
{
	struct regmap_config hub_master_regmap_config;
	struct fsi_device *fsi_dev = to_fsi_dev(dev);
	struct fsi_master_hub *hub;
	struct regmap *map;
	uint32_t reg, links;
	int rc;

	fsi_master_regmap_config(&hub_master_regmap_config);
	hub_master_regmap_config.reg_base = fsi_dev->addr;
	map = regmap_init_fsi(fsi_dev, &hub_master_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	rc = regmap_read(map, FSI_MVER, &reg);
	if (rc)
		goto err_regmap;

	links = (reg >> 8) & 0xff;
	dev_dbg(dev, "hub version %08x (%d links)\n", reg, links);

	rc = fsi_slave_claim_range(fsi_dev->slave, FSI_HUB_LINK_OFFSET,
			FSI_HUB_LINK_SIZE * links);
	if (rc) {
		dev_err(dev, "can't claim slave address range for links");
		goto err_regmap;
	}

	hub = kzalloc(sizeof(*hub), GFP_KERNEL);
	if (!hub) {
		rc = -ENOMEM;
		goto err_release;
	}

	hub->addr = FSI_HUB_LINK_OFFSET;
	hub->size = FSI_HUB_LINK_SIZE * links;
	hub->upstream = fsi_dev;

	hub->master.dev.parent = dev;
	hub->master.dev.release = hub_master_release;
	hub->master.dev.of_node = of_node_get(dev_of_node(dev));
	hub->master.map = map;

	hub->master.lbus_divider = 1;
	hub->master.idx = fsi_dev->slave->link + 1;
	hub->master.n_links = links;
	hub->master.flags = FSI_MASTER_FLAG_INTERRUPT;
	hub->master.read = hub_master_read;
	hub->master.write = hub_master_write;
	hub->master.send_break = hub_master_break;
	hub->master.link_enable = hub_master_link_enable;

	dev_set_drvdata(dev, hub);

	rc = fsi_master_init(&hub->master, fsi_device_local_bus_frequency(fsi_dev));
	if (rc)
		goto err_free;

	if (of_property_read_bool(dev->of_node, "interrupt-controller")) {
		struct device_node *parent = of_irq_find_parent(dev->of_node);

		if (parent) {
			struct irq_fwspec fwspec;
			unsigned int irq;

			fwspec.fwnode = of_node_to_fwnode(parent);
			fwspec.param_count = 1;
			fwspec.param[0] = (fsi_dev->slave->link * FSI_IRQ_COUNT) + 8;
			irq = irq_create_fwspec_mapping(&fwspec);
			if (irq) {
				unsigned int size = links * FSI_IRQ_COUNT;

				hub->irq_domain = irq_domain_add_linear(dev->of_node, size,
									&hub_master_irq_domain_ops,
									hub);

				if (hub->irq_domain) {
					rc = devm_request_irq(dev, irq, hub_master_irq, 0,
							      dev_name(dev), hub);
					if (rc) {
						dev_warn(dev, "failed to request irq:%u\n", irq);
						irq_domain_remove(hub->irq_domain);
						hub->irq_domain = NULL;
					} else {
						dev_info(dev, "enabling interrupts irq:%u\n", irq);
					}
				} else {
					dev_warn(dev, "failed to create irq domain\n");
				}
			}
		}
	}

	rc = fsi_master_register(&hub->master);
	if (rc)
		goto err_irq;

	/* At this point, fsi_master_register performs the device_initialize(),
	 * and holds the sole reference on master.dev. This means the device
	 * will be freed (via ->release) during any subsequent call to
	 * fsi_master_unregister.  We add our own reference to it here, so we
	 * can perform cleanup (in _remove()) without it being freed before
	 * we're ready.
	 */
	get_device(&hub->master.dev);
	return 0;

err_irq:
	if (hub->irq_domain)
		irq_domain_remove(hub->irq_domain);
err_free:
	kfree(hub);
err_release:
	fsi_slave_release_range(fsi_dev->slave, FSI_HUB_LINK_OFFSET,
			FSI_HUB_LINK_SIZE * links);
err_regmap:
	regmap_exit(map);
	return rc;
}

static int hub_master_remove(struct device *dev)
{
	struct fsi_master_hub *hub = dev_get_drvdata(dev);

	fsi_master_unregister(&hub->master);
	fsi_slave_release_range(hub->upstream->slave, hub->addr, hub->size);
	of_node_put(hub->master.dev.of_node);

	/*
	 * master.dev will likely be ->release()ed after this, which free()s
	 * the hub
	 */
	put_device(&hub->master.dev);

	return 0;
}

static const struct fsi_device_id hub_master_ids[] = {
	{
		.engine_type = FSI_ENGID_HUB_MASTER,
		.version = FSI_VERSION_ANY,
	},
	{ 0 }
};

static struct fsi_driver hub_master_driver = {
	.id_table = hub_master_ids,
	.drv = {
		.name = "fsi-master-hub",
		.bus = &fsi_bus_type,
		.probe = hub_master_probe,
		.remove = hub_master_remove,
	}
};

module_fsi_driver(hub_master_driver);
MODULE_LICENSE("GPL");
