// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha EN8811H MDIO device driver
 *
 * Until its firmware has been downloaded the EN8811H is not an Ethernet PHY,
 * it is an MD32 microcontroller waiting in its bootloader. Describing it as a
 * plain MDIO device lets the firmware be downloaded as soon as the files can
 * be read - in practice, once the filesystem holding them has been mounted -
 * and lets the PHY appear only after the chip is able to act as one.
 *
 * Copyright (C) 2026 Aleksei Sviridkin <f@lex.la>
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#include "../phy/air_phy_lib.h"

/*
 * Poll rather than defer probing: request_firmware_direct() has no usermode
 * helper fallback, so a rootfs that is not mounted yet fails immediately and
 * would keep the deferred-probe list spinning for the whole mount window.
 *
 * Never give up. The firmware can arrive arbitrarily late and still be worth
 * waiting for - installing the firmware package on a running system is a
 * normal thing to do - and a driver that had stopped looking would turn that
 * into a needless reboot. Back off to a slow poll instead, and leave a single
 * breadcrumb for the system that simply does not have the files.
 */
#define EN8811H_FW_POLL_MIN_MS	1000
#define EN8811H_FW_POLL_MAX_MS	30000
#define EN8811H_FW_WARN_MS	60000

struct en8811h_mcu {
	struct mdio_device *mdiodev;
	struct gpio_desc *reset_gpio;
	struct delayed_work fw_poll;
	unsigned int poll_ms;
	unsigned int waited_ms;
	u32 fw_version;
	bool warned;
};

static int en8811h_mcu_read(struct mii_bus *bus, int addr, int regnum)
{
	struct en8811h_mcu *mcu = bus->priv;
	struct mii_bus *parent = mcu->mdiodev->bus;
	int ret;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	mutex_lock_nested(&parent->mdio_lock, MDIO_MUTEX_NESTED);
	ret = __mdiobus_read(parent, addr, regnum);
	mutex_unlock(&parent->mdio_lock);

	return ret;
}

static int en8811h_mcu_write(struct mii_bus *bus, int addr, int regnum, u16 val)
{
	struct en8811h_mcu *mcu = bus->priv;
	struct mii_bus *parent = mcu->mdiodev->bus;
	int ret;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	mutex_lock_nested(&parent->mdio_lock, MDIO_MUTEX_NESTED);
	ret = __mdiobus_write(parent, addr, regnum, val);
	mutex_unlock(&parent->mdio_lock);

	return ret;
}

static int en8811h_mcu_read_c45(struct mii_bus *bus, int addr, int devad,
				int regnum)
{
	struct en8811h_mcu *mcu = bus->priv;
	struct mii_bus *parent = mcu->mdiodev->bus;
	int ret;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	mutex_lock_nested(&parent->mdio_lock, MDIO_MUTEX_NESTED);
	ret = __mdiobus_c45_read(parent, addr, devad, regnum);
	mutex_unlock(&parent->mdio_lock);

	return ret;
}

static int en8811h_mcu_write_c45(struct mii_bus *bus, int addr, int devad,
				 int regnum, u16 val)
{
	struct en8811h_mcu *mcu = bus->priv;
	struct mii_bus *parent = mcu->mdiodev->bus;
	int ret;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	mutex_lock_nested(&parent->mdio_lock, MDIO_MUTEX_NESTED);
	ret = __mdiobus_c45_write(parent, addr, devad, regnum, val);
	mutex_unlock(&parent->mdio_lock);

	return ret;
}

static int en8811h_mcu_bus_register(struct en8811h_mcu *mcu)
{
	struct device *dev = &mcu->mdiodev->dev;
	struct mii_bus *parent = mcu->mdiodev->bus;
	struct device_node *np;
	struct mii_bus *bus;
	int ret;

	np = of_get_child_by_name(dev->of_node, "mdio");
	if (!np) {
		dev_err(dev, "no mdio node describing the PHY\n");
		return -ENODEV;
	}

	bus = devm_mdiobus_alloc(dev);
	if (!bus) {
		of_node_put(np);
		return -ENOMEM;
	}

	bus->name = "airoha-en8811h";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	bus->priv = mcu;
	bus->parent = dev;

	if (parent->read) {
		bus->read = en8811h_mcu_read;
		bus->write = en8811h_mcu_write;
	}
	if (parent->read_c45) {
		bus->read_c45 = en8811h_mcu_read_c45;
		bus->write_c45 = en8811h_mcu_write_c45;
	}

	ret = devm_of_mdiobus_register(dev, bus, np);
	of_node_put(np);

	return ret;
}

static void en8811h_mcu_fw_poll(struct work_struct *work)
{
	struct en8811h_mcu *mcu = container_of(to_delayed_work(work),
					       struct en8811h_mcu, fw_poll);
	struct device *dev = &mcu->mdiodev->dev;
	int ret;

	/* The chip enumerates as a C22 PHY; MMD access is indirect */
	ret = air_en8811h_fw_download(mcu->mdiodev->bus, mcu->mdiodev->addr,
				      false, dev, &mcu->fw_version);
	if (!ret) {
		dev_dbg(dev, "firmware %08x running after %ums\n",
			mcu->fw_version, mcu->waited_ms);

		/* Unlike a missing firmware file, this does not resolve by
		 * itself, so unlike the poll there is no retry: surface it
		 * once and stop.
		 */
		ret = en8811h_mcu_bus_register(mcu);
		if (ret)
			dev_err(dev, "failed to register the PHY's bus: %pe\n",
				ERR_PTR(ret));
		return;
	}

	mcu->waited_ms += mcu->poll_ms;
	if (!mcu->warned && mcu->waited_ms >= EN8811H_FW_WARN_MS) {
		/* Missing files resolve by themselves once installed; a bus
		 * or register error will not, and deserves its own message.
		 */
		if (ret == -ENOENT)
			dev_warn(dev, "still waiting for %s and %s\n",
				 EN8811H_MD32_DM, EN8811H_MD32_DSP);
		else
			dev_warn(dev, "firmware download keeps failing: %pe\n",
				 ERR_PTR(ret));
		mcu->warned = true;
	}

	mcu->poll_ms = min(mcu->poll_ms * 2, EN8811H_FW_POLL_MAX_MS);
	queue_delayed_work(system_freezable_wq, &mcu->fw_poll,
			   msecs_to_jiffies(mcu->poll_ms));
}

static int en8811h_mcu_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct en8811h_mcu *mcu;
	struct device_node *np;
	u32 deassert_us = 0;

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->mdiodev = mdiodev;
	mdiodev_set_drvdata(mdiodev, mcu);

	/* The bus registration only needs this once the firmware runs, but
	 * a DT hole should fail the bind now, not as a work-item error a
	 * second after probe already returned success.
	 */
	np = of_get_child_by_name(dev->of_node, "mdio");
	if (!np)
		return dev_err_probe(dev, -ENODEV,
				     "no mdio node describing the PHY\n");
	of_node_put(np);

	/*
	 * The core only claims reset-gpios for devices flagged as PHYs
	 * (mdiobus_register_device()), so claim it here. Owning it at this
	 * level is the point: phy_detach() asserts the reset of the PHY it
	 * detaches, which would wipe firmware the MD32 holds in RAM.
	 */
	mcu->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(mcu->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(mcu->reset_gpio),
				     "failed to get reset GPIO\n");

	if (mcu->reset_gpio)
		gpiod_set_consumer_name(mcu->reset_gpio, "EN8811H reset");

	/*
	 * Firmware left running by the bootloader, or by a previous bind,
	 * lives in volatile RAM: the reset line must not be touched then.
	 * Only a chip still in its bootloader gets the clean reset cycle.
	 */
	if (air_en8811h_mcu_running(mdiodev->bus, mdiodev->addr, false)) {
		dev_dbg(dev, "MD32 already running, adopting it\n");
	} else if (mcu->reset_gpio) {
		u32 assert_us = 0;

		device_property_read_u32(dev, "reset-assert-us", &assert_us);
		device_property_read_u32(dev, "reset-deassert-us",
					 &deassert_us);

		gpiod_direction_output(mcu->reset_gpio, 1);
		if (assert_us)
			fsleep(assert_us);

		gpiod_set_value_cansleep(mcu->reset_gpio, 0);
		if (deassert_us)
			fsleep(deassert_us);
	}

	mcu->poll_ms = EN8811H_FW_POLL_MIN_MS;
	INIT_DELAYED_WORK(&mcu->fw_poll, en8811h_mcu_fw_poll);
	/* Freezable, so neither the file lookup nor the ~144KB MDIO
	 * download can land on a bus that is suspending. The download is
	 * long for a bound worker, but it runs once per firmware arrival.
	 */
	queue_delayed_work(system_freezable_wq, &mcu->fw_poll, 0);

	return 0;
}

static void en8811h_mcu_remove(struct mdio_device *mdiodev)
{
	struct en8811h_mcu *mcu = mdiodev_get_drvdata(mdiodev);

	cancel_delayed_work_sync(&mcu->fw_poll);
}

static const struct of_device_id en8811h_mcu_of_match[] = {
	{ .compatible = "airoha,en8811h-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, en8811h_mcu_of_match);

static struct mdio_driver en8811h_mcu_driver = {
	.probe = en8811h_mcu_probe,
	.remove = en8811h_mcu_remove,
	.mdiodrv.driver = {
		.name = "airoha-en8811h-mcu",
		.of_match_table = en8811h_mcu_of_match,
	},
};

mdio_module_driver(en8811h_mcu_driver);

MODULE_DESCRIPTION("Airoha EN8811H MDIO device driver");
MODULE_AUTHOR("Aleksei Sviridkin <f@lex.la>");
MODULE_LICENSE("GPL");
