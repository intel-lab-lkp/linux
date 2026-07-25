// SPDX-License-Identifier: GPL-2.0

#include <linux/spmi.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>

#include "core.h"

struct sn201202x {
	struct cd321x cd;
	struct completion select_completion;
	struct completion sleep_completion;
	struct completion wake_completion;
	struct spmi_device *sdev;
};

#define tps_to_sn(tps) container_of_const((tps), struct sn201202x, cd.tps)

static int regmap_sn201202x_select_reg(struct spmi_device *sdev, u8 reg)
{
	int err;
	u8 val;
	bool warned = false;
	struct tps6598x *tps = spmi_device_get_drvdata(sdev);
	struct sn201202x *sn = tps_to_sn(tps);

	reinit_completion(&sn->select_completion);
	err = spmi_register_zero_write(sdev, reg);
	if (err)
		return err;

	if (!wait_for_completion_timeout(&sn->select_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;

	while (1) {
		err = spmi_register_read(sdev, 0, &val);
		if (err)
			return err;
		if (val == (reg | 0x80)) {
			if (!warned) {
				dev_warn(tps->dev,
					 "Got interrupt but selection not complete?\n");
				warned = true;
			}
			msleep(20);
			continue;
		}
		if (val == reg)
			break;
		return -EIO;
	}

	return 0;
}

static int regmap_sn201202x_read(void *context,
				 const void *reg, size_t reg_size,
				 void *val, size_t val_size)
{
	int err;
	unsigned int offset = 0x20;
	size_t len;
	u8 addr;

	WARN_ON(reg_size != 1);
	WARN_ON(val_size > 0x40);

	addr = *(u8 *)reg;

	err = regmap_sn201202x_select_reg(context, addr);
	if (err)
		return err;

	while (val_size) {
		len = min_t(size_t, val_size, 16);
		err = spmi_ext_register_read(context, offset, val, len);
		if (err)
			return err;
		offset += len;
		val += len;
		val_size -= len;
	}

	return 0;
}

static int regmap_sn201202x_write(void *context, const void *data,
				  size_t count)
{
	int err = 0;
	unsigned int offset = 0xa0;
	size_t len;
	u8 addr;

	WARN_ON(count < 1);

	addr = *(u8 *)data;
	data += 1;
	count -= 1;

	WARN_ON(count > 0x40);

	err = regmap_sn201202x_select_reg(context, addr);
	if (err)
		return err;

	while (count) {
		len = min_t(size_t, count, 16);
		err = spmi_ext_register_write(context, offset, data, len);
		if (err)
			return err;
		offset += len;
		data += len;
		count -= len;
	}

	return err;
}

static irqreturn_t sn201202x_irq(int irq, void *data)
{
	struct completion *c = data;

	complete(c);
	return IRQ_HANDLED;
}

static const struct regmap_bus regmap_sn201202x = {
	.read				= regmap_sn201202x_read,
	.write				= regmap_sn201202x_write,
	.reg_format_endian_default	= REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default	= REGMAP_ENDIAN_NATIVE,
};

static struct regmap *__devm_regmap_init_sn201202x(struct spmi_device *sdev,
						   const struct regmap_config *config,
						   struct lock_class_key *lock_key,
						   const char *lock_name)
{
	return __devm_regmap_init(&sdev->dev, &regmap_sn201202x, sdev, config,
				  lock_key, lock_name);
}

#define devm_regmap_init_sn201202x(dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_sn201202x, #config,	\
				dev, config)

static const struct tipd_data sn201202x_data = {
	.irq_handler = cd321x_interrupt,
	.irq_mask1 = APPLE_CD_REG_INT_POWER_STATUS_UPDATE |
		     APPLE_CD_REG_INT_DATA_STATUS_UPDATE |
		     APPLE_CD_REG_INT_PLUG_EVENT,
	.tps_struct_size = sizeof(struct sn201202x),
	.remove = cd321x_remove,
	.register_port = cd321x_register_port,
	.unregister_port = cd321x_unregister_port,
	.trace_data_status = trace_cd321x_data_status,
	.trace_power_status = trace_tps6598x_power_status,
	.trace_status = trace_tps6598x_status,
	.init = cd321x_init,
	.read_data_status = cd321x_read_data_status,
	.reset = cd321x_reset,
	.switch_power_state = cd321x_switch_power_state,
	.connect = cd321x_connect,
};

static const struct of_device_id sn201202x_of_match[] = {
	{ .compatible = "apple,sn201202x", &sn201202x_data},
	{}
};

static int sn201202x_probe(struct spmi_device *device)
{
	const struct of_device_id *match;
	const struct tipd_data *data;
	struct sn201202x *sn;
	struct tps6598x *tps;
	int irq_select, irq_sleep, irq_wake;
	int ret;

	match = of_match_device(sn201202x_of_match, &device->dev);
	if (!match)
		return -EINVAL;
	data = match->data;

	sn = devm_kzalloc(&device->dev, data->tps_struct_size, GFP_KERNEL);
	if (!sn)
		return -ENOMEM;
	sn->sdev = device;
	tps = &sn->cd.tps;

	mutex_init(&tps->lock);
	tps->dev = &device->dev;
	tps->data = data;

	tps->irq = of_irq_get_byname(device->dev.of_node, "irq");
	if (tps->irq < 0)
		return tps->irq;
	irq_select = of_irq_get_byname(device->dev.of_node, "select");
	if (irq_select < 0)
		return irq_select;
	irq_sleep = of_irq_get_byname(device->dev.of_node, "sleep");
	if (irq_sleep < 0)
		return irq_sleep;
	irq_wake = of_irq_get_byname(device->dev.of_node, "wake");
	if (irq_wake < 0)
		return irq_wake;

	init_completion(&sn->select_completion);
	init_completion(&sn->sleep_completion);
	init_completion(&sn->wake_completion);

	ret = devm_request_threaded_irq(&device->dev, irq_select, NULL, sn201202x_irq,
					IRQF_ONESHOT, NULL, &sn->select_completion);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(&device->dev, irq_sleep, NULL, sn201202x_irq,
					IRQF_ONESHOT, NULL, &sn->sleep_completion);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(&device->dev, irq_wake, NULL, sn201202x_irq,
					IRQF_ONESHOT, NULL, &sn->wake_completion);
	if (ret)
		return ret;

	tps->regmap = devm_regmap_init_sn201202x(device, &tps6598x_regmap_config);
	if (IS_ERR(tps->regmap))
		return PTR_ERR(tps->regmap);

	ret = spmi_command_wakeup(device);
	if (ret)
		return ret;
	if (!wait_for_completion_timeout(&sn->wake_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;

	spmi_device_set_drvdata(device, tps);
	return tps6598x_probe(tps);
}

static void sn201202x_remove(struct spmi_device *device)
{
	struct tps6598x *tps = spmi_device_get_drvdata(device);

	tps6598x_remove(tps);
}

static int __maybe_unused sn201202x_resume(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	int err;

	err = spmi_command_wakeup(sn->sdev);
	if (err)
		return err;
	if (!wait_for_completion_timeout(&sn->wake_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;
	return tps6598x_resume(dev);
}

static int __maybe_unused sn201202x_suspend(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	int err;

	err = tps6598x_suspend(dev);
	if (err)
		return err;
	err = spmi_command_sleep(sn->sdev);
	if (err)
		return err;
	if (!wait_for_completion_timeout(&sn->sleep_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;
	return 0;
}

MODULE_DEVICE_TABLE(of, sn201202x_of_match);

static const struct dev_pm_ops sn201202x_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sn201202x_suspend, sn201202x_resume)
};

static struct spmi_driver sn201202x_driver = {
	.driver = {
		.name = "sn201202x",
		.pm = &sn201202x_pm_ops,
		.of_match_table = sn201202x_of_match,
	},
	.probe = sn201202x_probe,
	.remove = sn201202x_remove,
};
module_spmi_driver(sn201202x_driver);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TI SN201202x USB Power Delivery Controller Driver");
