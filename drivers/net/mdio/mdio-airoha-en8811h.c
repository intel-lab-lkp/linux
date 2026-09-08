// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha EN8811H MDIO device driver
 *
 * The EN8811H is an MD32 microcontroller until firmware is downloaded into
 * it, and only then an Ethernet PHY.
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
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#include <linux/mdio/mdio-airoha-en8811h.h>

#define EN8811H_FW_POLL_MIN_MS	1000
#define EN8811H_FW_POLL_MAX_MS	30000
#define EN8811H_FW_WARN_MS	60000

struct en8811h_mcu {
	struct mdio_device *mdiodev;
	struct gpio_desc *reset_gpio;
	struct delayed_work fw_poll;
	struct mii_bus *bus;
	unsigned int poll_ms;
	unsigned int waited_ms;
	u32 fw_version;
	bool warned;
	bool fw_running;
};

static int en8811h_mcu_read(struct mii_bus *bus, int addr, int regnum)
{
	struct en8811h_mcu *mcu = bus->priv;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	return mdiobus_read_nested(mcu->mdiodev->bus, addr, regnum);
}

static int en8811h_mcu_write(struct mii_bus *bus, int addr, int regnum, u16 val)
{
	struct en8811h_mcu *mcu = bus->priv;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	return mdiobus_write_nested(mcu->mdiodev->bus, addr, regnum, val);
}

static int en8811h_mcu_read_c45(struct mii_bus *bus, int addr, int devad,
				int regnum)
{
	struct en8811h_mcu *mcu = bus->priv;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	return mdiobus_c45_read_nested(mcu->mdiodev->bus, addr, devad, regnum);
}

static int en8811h_mcu_write_c45(struct mii_bus *bus, int addr, int devad,
				 int regnum, u16 val)
{
	struct en8811h_mcu *mcu = bus->priv;

	if (addr != mcu->mdiodev->addr)
		return -ENODEV;

	return mdiobus_c45_write_nested(mcu->mdiodev->bus, addr, devad, regnum,
					val);
}

static int en8811h_mcu_bus_register(struct en8811h_mcu *mcu)
{
	struct mii_bus *parent = mcu->mdiodev->bus;
	struct device *dev = &mcu->mdiodev->dev;
	struct device_node *np;
	struct mii_bus *bus;
	int ret;

	np = of_get_child_by_name(dev->of_node, "mdio");
	if (!np)
		return -ENODEV;

	/* Not devm: this is retried, and a devm bus would only be freed at
	 * detach.
	 */
	bus = mdiobus_alloc();
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

	ret = of_mdiobus_register(bus, np);
	of_node_put(np);
	if (!ret && !mdiobus_get_phy(bus, mcu->mdiodev->addr)) {
		/* An ID read that failed leaves the bus registered and the
		 * PHY absent; of_mdiobus_register() returns 0 either way.
		 */
		mdiobus_unregister(bus);
		ret = -ENODEV;
	}
	if (ret) {
		mdiobus_free(bus);
		return ret;
	}

	mcu->bus = bus;
	return 0;
}

/* Serialise with the PHY below, which reaches the same registers under
 * this lock through phy_select_page().
 */
static struct mii_bus *en8811h_mcu_chip_lock(struct en8811h_mcu *mcu)
{
	struct mii_bus *bus = mcu->bus;

	if (bus)
		mutex_lock(&bus->mdio_lock);

	return bus;
}

static void en8811h_mcu_chip_unlock(struct mii_bus *bus)
{
	if (bus)
		mutex_unlock(&bus->mdio_lock);
}

static void en8811h_mcu_fw_poll(struct work_struct *work)
{
	struct en8811h_mcu *mcu = container_of(to_delayed_work(work),
					       struct en8811h_mcu, fw_poll);
	struct device *dev = &mcu->mdiodev->dev;
	struct mii_bus *chip;
	int ret;

	if (!mcu->fw_running) {
		chip = en8811h_mcu_chip_lock(mcu);
		ret = air_en8811h_fw_download(mcu->mdiodev, &mcu->fw_version,
					      !!chip);
		en8811h_mcu_chip_unlock(chip);
		if (ret < 0)
			goto retry;

		dev_dbg(dev, "firmware %08x running after %ums\n",
			mcu->fw_version, mcu->waited_ms);
		ret = firmware_request_cache(dev, EN8811H_MD32_DM) ?:
		      firmware_request_cache(dev, EN8811H_MD32_DSP);
		if (ret)
			dev_dbg(dev, "not cached, resume will want the files: %pe\n",
				ERR_PTR(ret));
		mcu->fw_running = true;
		mcu->poll_ms = EN8811H_FW_POLL_MIN_MS;
		mcu->waited_ms = 0;
		mcu->warned = false;
	}

	/* Resume re-runs the download, so the bus can already be here, and
	 * fwnode_mdio defers while the PHY node's interrupt controller is
	 * missing, so a failure is not necessarily permanent.
	 */
	ret = mcu->bus ? 0 : en8811h_mcu_bus_register(mcu);
	if (!ret)
		return;

retry:
	if (!mcu->warned && mcu->waited_ms >= EN8811H_FW_WARN_MS) {
		if (!mcu->fw_running)
			dev_warn(dev, "no firmware after %ums of waiting for %s and %s: %pe\n",
				 mcu->waited_ms, EN8811H_MD32_DM,
				 EN8811H_MD32_DSP, ERR_PTR(ret));
		else
			dev_warn(dev, "no PHY at address %d, %ums after the firmware started: %pe\n",
				 mcu->mdiodev->addr, mcu->waited_ms,
				 ERR_PTR(ret));
		mcu->warned = true;
	}

	/* Past its budget, only a deferral will change on its own: every
	 * other error repeats a bus registration, its message from the MDIO
	 * core and its uevents for the uptime. Firmware files are the
	 * exception, since they can be installed at any time.
	 */
	if (mcu->fw_running && ret != -EPROBE_DEFER &&
	    mcu->waited_ms >= EN8811H_FW_WARN_MS)
		return;

	mcu->poll_ms = min(mcu->poll_ms * 2, EN8811H_FW_POLL_MAX_MS);
	/* Count the sleep ahead: the first run was immediate. */
	mcu->waited_ms += mcu->poll_ms;
	queue_delayed_work(system_freezable_wq, &mcu->fw_poll,
			   msecs_to_jiffies(mcu->poll_ms));
}

/* The firmware lives in volatile RAM: no reset while the MD32 reports ready. */
static void en8811h_mcu_reset_if_dormant(struct en8811h_mcu *mcu, bool nested)
{
	struct mdio_device *mdiodev = mcu->mdiodev;
	struct device *dev = &mdiodev->dev;
	u32 assert_us = 0, deassert_us = 0;
	int ret;

	ret = air_en8811h_mcu_running(mdiodev, nested);
	if (ret > 0) {
		dev_dbg(dev, "MD32 already running, leaving reset alone\n");
		return;
	}

	if (!mcu->reset_gpio)
		return;

	/* A failed read is not a dormant chip, so do not touch a line that
	 * is already deasserted. An asserted one is why the read failed.
	 */
	if (ret < 0 && gpiod_get_value_cansleep(mcu->reset_gpio) <= 0) {
		dev_dbg(dev, "MD32 state unknown (%d), leaving reset alone\n",
			ret);
		return;
	}

	device_property_read_u32(dev, "reset-assert-us", &assert_us);
	device_property_read_u32(dev, "reset-deassert-us", &deassert_us);

	ret = gpiod_direction_output(mcu->reset_gpio, 1);
	if (ret) {
		dev_warn(dev, "reset not asserted: %pe\n", ERR_PTR(ret));
		return;
	}

	if (assert_us)
		fsleep(assert_us);

	gpiod_set_value_cansleep(mcu->reset_gpio, 0);
	if (deassert_us)
		fsleep(deassert_us);
}

static int en8811h_mcu_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct en8811h_mcu *mcu;
	struct device_node *np;

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->mdiodev = mdiodev;
	mdiodev_set_drvdata(mdiodev, mcu);

	/* Registration needs this only once the firmware runs, but a DT
	 * hole should fail the bind now, not as a work-item error later.
	 */
	np = of_get_child_by_name(dev->of_node, "mdio");
	if (!np)
		return dev_err_probe(dev, -ENODEV,
				     "no mdio node describing the PHY\n");
	of_node_put(np);

	/* The core claims reset-gpios only for devices flagged as PHYs. */
	mcu->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(mcu->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(mcu->reset_gpio),
				     "failed to get reset GPIO\n");

	if (mcu->reset_gpio)
		gpiod_set_consumer_name(mcu->reset_gpio, "EN8811H reset");

	en8811h_mcu_reset_if_dormant(mcu, false);

	mcu->poll_ms = EN8811H_FW_POLL_MIN_MS;
	INIT_DELAYED_WORK(&mcu->fw_poll, en8811h_mcu_fw_poll);
	/* Freezable: neither the file lookup nor the download may land on
	 * a suspending bus.
	 */
	queue_delayed_work(system_freezable_wq, &mcu->fw_poll, 0);

	return 0;
}

static void en8811h_mcu_remove(struct mdio_device *mdiodev)
{
	struct en8811h_mcu *mcu = mdiodev_get_drvdata(mdiodev);

	cancel_delayed_work_sync(&mcu->fw_poll);
	if (mcu->bus) {
		mdiobus_unregister(mcu->bus);
		mdiobus_free(mcu->bus);
		mcu->bus = NULL;
	}
}

static int en8811h_mcu_resume(struct device *dev)
{
	struct en8811h_mcu *mcu = dev_get_drvdata(dev);
	struct mii_bus *chip;
	int ret;

	/* Nothing to redo: the poll is armed and thaws with everything else. */
	if (!mcu->fw_running)
		return 0;

	/* Not on the workqueue: a DSA port's PHY resumes right after this
	 * one and calls phy_init_hw(), which needs the firmware by then.
	 */
	chip = en8811h_mcu_chip_lock(mcu);
	en8811h_mcu_reset_if_dormant(mcu, !!chip);
	ret = air_en8811h_fw_download(mcu->mdiodev, &mcu->fw_version, !!chip);
	en8811h_mcu_chip_unlock(chip);
	if (ret < 0) {
		/* The reload restores this chip, not the PHY below it: its
		 * own resume has already failed by then and nothing calls
		 * phy_init_hw() twice.
		 */
		dev_err(dev, "firmware not restored, reloading: %pe\n",
			ERR_PTR(ret));
		mcu->fw_running = false;
		mcu->poll_ms = EN8811H_FW_POLL_MIN_MS;
		mcu->waited_ms = 0;
		mcu->warned = false;
		queue_delayed_work(system_freezable_wq, &mcu->fw_poll, 0);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(en8811h_mcu_pm_ops, NULL, en8811h_mcu_resume);

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
		.pm = pm_sleep_ptr(&en8811h_mcu_pm_ops),
		/* Tearing the child bus down under an attached PHY is not
		 * something this driver can make safe on its own.
		 */
		.suppress_bind_attrs = true,
	},
};

mdio_module_driver(en8811h_mcu_driver);

MODULE_FIRMWARE(EN8811H_MD32_DM);
MODULE_FIRMWARE(EN8811H_MD32_DSP);

MODULE_DESCRIPTION("Airoha EN8811H MDIO device driver");
MODULE_AUTHOR("Aleksei Sviridkin <f@lex.la>");
MODULE_LICENSE("GPL");
