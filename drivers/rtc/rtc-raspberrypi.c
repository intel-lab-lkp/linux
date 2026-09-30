// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/**
 * rtc-raspberrypi.c
 *
 * RTC driver using firmware mailbox
 * Supports battery backed RTC and wake alarms
 *
 * Based on rtc-meson-vrtc by Neil Armstrong
 *
 * Copyright (c) 2023, Raspberry Pi Ltd.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/of.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define RPI_FIRMWARE_GET_RTC_REG 0x00030087
#define RPI_FIRMWARE_SET_RTC_REG 0x00038087

enum {
	RTC_TIME,
	RTC_ALARM,
	RTC_ALARM_PENDING,
	RTC_ALARM_ENABLE,
	RTC_BBAT_CHG_VOLTS,
	RTC_BBAT_CHG_VOLTS_MIN,
	RTC_BBAT_CHG_VOLTS_MAX,
	RTC_BBAT_VOLTS
};

struct rpi_rtc_data {
	struct rtc_device *rtc;
	struct rpi_firmware *fw;
	u32 bbat_vchg_millivolts;
	u32 bbat_vchg_min_millivolts;
	u32 bbat_vchg_max_millivolts;
};

static int rpi_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_TIME };
	int err;

	err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_GET_RTC_REG,
				    &data, sizeof(data));
	rtc_time64_to_tm(le32_to_cpu(data[1]), tm);
	return err;
}

static int rpi_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_TIME, cpu_to_le32(rtc_tm_to_time64(tm)) };

	return rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_SET_RTC_REG,
				     &data, sizeof(data));
}

static int rpi_rtc_alarm_irq_is_enabled(struct device *dev, unsigned char *enabled)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_ALARM_ENABLE };
	s32 err = 0;

	err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_GET_RTC_REG,
				    &data, sizeof(data));
	*enabled = data[1] & 0x1;
	return err;
}

static int rpi_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_ALARM_ENABLE, enabled };

	return rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_SET_RTC_REG,
				     &data, sizeof(data));
}

static int rpi_rtc_alarm_clear_pending(struct device *dev)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_ALARM_PENDING, 1 };

	return rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_SET_RTC_REG,
				     &data, sizeof(data));
}

static int rpi_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_ALARM };
	s32 err = 0;

	err = rpi_rtc_alarm_irq_is_enabled(dev, &alarm->enabled);
	if (!err)
		err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_GET_RTC_REG,
					    &data, sizeof(data));
	rtc_time64_to_tm(data[1], &alarm->time);

	return err;
}

static int rpi_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_ALARM, rtc_tm_to_time64(&alarm->time) };
	int err;

	err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_SET_RTC_REG,
				    &data, sizeof(data));

	if (err == 0)
		err = rpi_rtc_alarm_irq_enable(dev, alarm->enabled);

	return err;
}

static const struct rtc_class_ops rpi_rtc_ops = {
	.read_time = rpi_rtc_read_time,
	.set_time = rpi_rtc_set_time,
	.read_alarm = rpi_rtc_read_alarm,
	.set_alarm = rpi_rtc_set_alarm,
	.alarm_irq_enable = rpi_rtc_alarm_irq_enable,
};

static void rpi_rtc_set_limits(struct device *dev)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_BBAT_CHG_VOLTS_MIN };

	int err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_GET_RTC_REG,
					&data, sizeof(data));
	if (err == 0)
		vrtc->bbat_vchg_min_millivolts = data[1] / 1000U;

	data[0] = RTC_BBAT_CHG_VOLTS_MAX;
	data[1] = 0;
	err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_GET_RTC_REG,
				    &data, sizeof(data));
	if (err == 0)
		vrtc->bbat_vchg_max_millivolts = data[1] / 1000U;
}

static int rpi_rtc_set_charge_voltage(struct device *dev)
{
	struct rpi_rtc_data *vrtc = dev_get_drvdata(dev);
	u32 data[2] = { RTC_BBAT_CHG_VOLTS, vrtc->bbat_vchg_millivolts * 1000U };
	int err;

	if (vrtc->bbat_vchg_millivolts != 0 &&
	    (vrtc->bbat_vchg_millivolts < vrtc->bbat_vchg_min_millivolts ||
	    vrtc->bbat_vchg_millivolts > vrtc->bbat_vchg_max_millivolts)) {
		dev_warn(dev, "trickle charge voltage %umV is outside of the supported range (%umV - %umV)\n",
			 vrtc->bbat_vchg_millivolts,
			 vrtc->bbat_vchg_min_millivolts,
			 vrtc->bbat_vchg_max_millivolts);
		return -EINVAL;
	}

	err = rpi_firmware_property(vrtc->fw, RPI_FIRMWARE_SET_RTC_REG,
				    &data, sizeof(data));

	if (err)
		dev_err(dev, "failed to set trickle charge voltage to %umV: %d\n",
			vrtc->bbat_vchg_millivolts, err);
	else if (vrtc->bbat_vchg_millivolts)
		dev_info(dev, "trickle charging enabled at %umV\n",
			 vrtc->bbat_vchg_millivolts);

	return err;
}

static int rpi_rtc_probe(struct platform_device *pdev)
{
	struct rpi_rtc_data *vrtc;
	struct device *dev = &pdev->dev;
	struct rpi_firmware *firmware;
	int ret;

	// Get the firmware device from the parent device
	firmware = devm_rpi_firmware_get(dev, dev->parent->of_node);

	if (!firmware)
		return -EPROBE_DEFER;

	vrtc = devm_kzalloc(dev, sizeof(*vrtc), GFP_KERNEL);
	if (!vrtc)
		return -ENOMEM;

	vrtc->fw = firmware;

	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, vrtc);

	vrtc->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(vrtc->rtc))
		return PTR_ERR(vrtc->rtc);

	vrtc->rtc->range_max = U32_MAX;           /* 2106-02-07 */

	set_bit(RTC_FEATURE_ALARM_WAKEUP_ONLY, vrtc->rtc->features);
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, vrtc->rtc->features);

	vrtc->rtc->ops = &rpi_rtc_ops;

	rpi_rtc_alarm_clear_pending(dev);

	/*
	 * The trickle-voltage property lives on the parent firmware node
	 * because we deliberately do not create a DT child node just to
	 * instantiate this driver (see DT maintainer guidance).
	 * The firmware driver registers us as a platform device at runtime.
	 */
	vrtc->bbat_vchg_millivolts = 0;
	of_property_read_u32(dev->parent->of_node, "trickle-voltage-millivolt",
			     &vrtc->bbat_vchg_millivolts);

	rpi_rtc_set_limits(dev);
	rpi_rtc_set_charge_voltage(dev);

	return devm_rtc_register_device(vrtc->rtc);
}

static const struct of_device_id rpi_rtc_dt_match[] = {
	{ .compatible = "raspberrypi,firmware-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpi_rtc_dt_match);

static struct platform_driver rpi_rtc_driver = {
	.driver = {
		.name = "raspberrypi-rtc",
		.of_match_table = of_match_ptr(rpi_rtc_dt_match),
	},
	.probe = rpi_rtc_probe
};

module_platform_driver(rpi_rtc_driver);

MODULE_AUTHOR("Jonathan Bell <jonathan@raspberrypi.com>");
MODULE_AUTHOR("Sander Speetjens <sander.speetjens@gmail.com>");
MODULE_DESCRIPTION("Raspberry Pi RTC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:raspberrypi-rtc");
