// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSI hub master driver
 *
 * Copyright (C) IBM Corporation 2016
 */

#include <linux/delay.h>
#include <linux/fsi.h>
#include <linux/module.h>
#include <linux/of.h>
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
	struct fsi_device	*upstream;
	uint32_t		addr, size;	/* slave-relative addr of */
						/* master address space */
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

static void hub_master_release(struct device *dev)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(to_fsi_master(dev));

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

	dev_set_drvdata(dev, hub);

	rc = fsi_master_init(&hub->master, fsi_device_local_bus_frequency(fsi_dev));
	if (rc)
		goto err_free;

	rc = fsi_master_register(&hub->master);
	if (rc)
		goto err_free;

	/* At this point, fsi_master_register performs the device_initialize(),
	 * and holds the sole reference on master.dev. This means the device
	 * will be freed (via ->release) during any subsequent call to
	 * fsi_master_unregister.  We add our own reference to it here, so we
	 * can perform cleanup (in _remove()) without it being freed before
	 * we're ready.
	 */
	get_device(&hub->master.dev);
	return 0;

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
