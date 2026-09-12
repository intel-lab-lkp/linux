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
	unsigned int poll_ms;
	unsigned int waited_ms;
	u32 fw_version;
	bool warned;
};

static void en8811h_mcu_fw_poll(struct work_struct *work)
{
	struct en8811h_mcu *mcu = container_of(to_delayed_work(work),
					       struct en8811h_mcu, fw_poll);
	struct device *dev = &mcu->mdiodev->dev;
	int cached, ret;

	ret = air_en8811h_fw_download(mcu->mdiodev, &mcu->fw_version);
	if (ret >= 0) {
		dev_dbg(dev, "firmware %08x running after %ums\n",
			mcu->fw_version, mcu->waited_ms);
		cached = firmware_request_cache(dev, EN8811H_MD32_DM);
		ret = firmware_request_cache(dev, EN8811H_MD32_DSP) ?: cached;
		if (ret)
			dev_warn(dev, "not cached, resume will read the files off a filesystem: %pe\n",
				 ERR_PTR(ret));
		return;
	}

	if (!mcu->warned && mcu->waited_ms >= EN8811H_FW_WARN_MS) {
		if (ret == -ENOENT)
			dev_warn(dev, "still waiting for %s and %s\n",
				 EN8811H_MD32_DM, EN8811H_MD32_DSP);
		else
			dev_warn(dev, "firmware download keeps failing: %pe\n",
				 ERR_PTR(ret));
		mcu->warned = true;
	}

	/* Count the sleep ahead: the first run was immediate. */
	mcu->waited_ms += mcu->poll_ms;
	queue_delayed_work(system_freezable_wq, &mcu->fw_poll,
			   msecs_to_jiffies(mcu->poll_ms));
	mcu->poll_ms = min(mcu->poll_ms * 2, EN8811H_FW_POLL_MAX_MS);
}

/* The firmware lives in volatile RAM: no reset while the MD32 reports ready. */
static void en8811h_mcu_reset_unless_running(struct en8811h_mcu *mcu)
{
	struct mdio_device *mdiodev = mcu->mdiodev;
	struct device *dev = &mdiodev->dev;
	u32 assert_us = 0, deassert_us = 0;
	int ret;

	ret = air_en8811h_mcu_running(mdiodev);
	if (ret > 0) {
		dev_dbg(dev, "MD32 already running, leaving reset alone\n");
		return;
	}

	if (!mcu->reset_gpio)
		return;

	/* A failed read does not mean the firmware is gone: leave a
	 * deasserted line alone, and clear an asserted one before giving up.
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

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->mdiodev = mdiodev;
	mdiodev_set_drvdata(mdiodev, mcu);

	/* The core claims reset-gpios only for devices flagged as PHYs. */
	mcu->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(mcu->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(mcu->reset_gpio),
				     "failed to get reset GPIO\n");

	if (mcu->reset_gpio)
		gpiod_set_consumer_name(mcu->reset_gpio, "EN8811H reset");

	en8811h_mcu_reset_unless_running(mcu);

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
}

static int en8811h_mcu_resume(struct device *dev)
{
	struct en8811h_mcu *mcu = dev_get_drvdata(dev);
	int ret;

	/* Synchronous: the poll's workqueue is freezable and thaws only
	 * after the resume callbacks have run.
	 */
	en8811h_mcu_reset_unless_running(mcu);
	ret = air_en8811h_fw_download(mcu->mdiodev, &mcu->fw_version);
	if (ret < 0)
		dev_err(dev, "firmware not restored: %pe\n", ERR_PTR(ret));

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
	},
};

mdio_module_driver(en8811h_mcu_driver);

MODULE_FIRMWARE(EN8811H_MD32_DM);
MODULE_FIRMWARE(EN8811H_MD32_DSP);

MODULE_DESCRIPTION("Airoha EN8811H MDIO device driver");
MODULE_AUTHOR("Aleksei Sviridkin <f@lex.la>");
MODULE_LICENSE("GPL");
