// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L50 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2023 Cirrus Logic, Inc.
 *
 */

#include <linux/gpio/consumer.h>
#include <linux/mfd/core.h>
#include <linux/mfd/cs40l50.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

static const struct mfd_cell cs40l50_devs[] = {
	{
		.name = "cs40l50-vibra",
	},
};

const struct regmap_config cs40l50_regmap = {
	.reg_bits =		32,
	.reg_stride =		4,
	.val_bits =		32,
	.reg_format_endian =	REGMAP_ENDIAN_BIG,
	.val_format_endian =	REGMAP_ENDIAN_BIG,
};
EXPORT_SYMBOL_GPL(cs40l50_regmap);

static struct regulator_bulk_data cs40l50_supplies[] = {
	{
		.supply = "vp",
	},
	{
		.supply = "vio",
	},
};

static int cs40l50_handle_f0_est_done(struct cs40l50_private *cs40l50)
{
	u32 f_zero;
	int error;

	error = regmap_read(cs40l50->regmap, CS40L50_F0_ESTIMATION, &f_zero);
	if (error)
		return error;

	return regmap_write(cs40l50->regmap, CS40L50_F0_STORED, f_zero);
}

static int cs40l50_handle_redc_est_done(struct cs40l50_private *cs40l50)
{
	int error, fractional, integer, stored;
	u32 redc;

	error = regmap_read(cs40l50->regmap, CS40L50_RE_EST_STATUS, &redc);
	if (error)
		return error;

	error = regmap_write(cs40l50->regmap, CS40L50_REDC_ESTIMATION, redc);
	if (error)
		return error;

	/* Convert from Q8.15 to (Q7.17 * 29/240) */
	integer = ((redc >> 15) & 0xFF) << 17;
	fractional = (redc & 0x7FFF) * 4;
	stored = (integer | fractional) * 29 / 240;

	return regmap_write(cs40l50->regmap, CS40L50_REDC_STORED, stored);
}

static int cs40l50_error_release(struct cs40l50_private *cs40l50)
{
	int error;

	error = regmap_write(cs40l50->regmap, CS40L50_ERR_RLS,
			     CS40L50_GLOBAL_ERR_RLS);
	if (error)
		return error;

	return regmap_write(cs40l50->regmap, CS40L50_ERR_RLS, 0);
}

static int cs40l50_mailbox_read_next(struct cs40l50_private *cs40l50, u32 *val)
{
	u32 rd_ptr, wt_ptr;
	int error;

	error = regmap_read(cs40l50->regmap, CS40L50_MBOX_QUEUE_WT, &wt_ptr);
	if (error)
		return error;

	error = regmap_read(cs40l50->regmap, CS40L50_MBOX_QUEUE_RD, &rd_ptr);
	if (error)
		return error;

	if (wt_ptr == rd_ptr) {
		*val = 0;
		return 0;
	}

	error = regmap_read(cs40l50->regmap, rd_ptr, val);
	if (error)
		return error;

	rd_ptr += sizeof(u32);
	if (rd_ptr > CS40L50_MBOX_QUEUE_END)
		rd_ptr = CS40L50_MBOX_QUEUE_BASE;

	return regmap_write(cs40l50->regmap, CS40L50_MBOX_QUEUE_RD, rd_ptr);
}

static irqreturn_t cs40l50_process_mbox(int irq, void *data)
{
	struct cs40l50_private *cs40l50 = data;
	int error = 0;
	u32 val;

	mutex_lock(&cs40l50->lock);

	while (!cs40l50_mailbox_read_next(cs40l50, &val)) {
		switch (val) {
		case 0:
			mutex_unlock(&cs40l50->lock);
			dev_dbg(cs40l50->dev, "Reached end of queue\n");
			return IRQ_HANDLED;
		case CS40L50_MBOX_HAPTIC_TRIGGER_GPIO:
			dev_dbg(cs40l50->dev, "Mailbox: TRIGGER_GPIO\n");
			break;
		case CS40L50_MBOX_HAPTIC_TRIGGER_MBOX:
			dev_dbg(cs40l50->dev, "Mailbox: TRIGGER_MBOX\n");
			break;
		case CS40L50_MBOX_HAPTIC_TRIGGER_I2S:
			dev_dbg(cs40l50->dev, "Mailbox: TRIGGER_I2S\n");
			break;
		case CS40L50_MBOX_HAPTIC_COMPLETE_MBOX:
			dev_dbg(cs40l50->dev, "Mailbox: COMPLETE_MBOX\n");
			break;
		case CS40L50_MBOX_HAPTIC_COMPLETE_GPIO:
			dev_dbg(cs40l50->dev, "Mailbox: COMPLETE_GPIO\n");
			break;
		case CS40L50_MBOX_HAPTIC_COMPLETE_I2S:
			dev_dbg(cs40l50->dev, "Mailbox: COMPLETE_I2S\n");
			break;
		case CS40L50_MBOX_F0_EST_START:
			dev_dbg(cs40l50->dev, "Mailbox: F0_EST_START\n");
			break;
		case CS40L50_MBOX_F0_EST_DONE:
			dev_dbg(cs40l50->dev, "Mailbox: F0_EST_DONE\n");
			error = cs40l50_handle_f0_est_done(cs40l50);
			if (error)
				goto out_mutex;
			break;
		case CS40L50_MBOX_REDC_EST_START:
			dev_dbg(cs40l50->dev, "Mailbox: REDC_EST_START\n");
			break;
		case CS40L50_MBOX_REDC_EST_DONE:
			dev_dbg(cs40l50->dev, "Mailbox: REDC_EST_DONE\n");
			error = cs40l50_handle_redc_est_done(cs40l50);
			if (error)
				goto out_mutex;
			break;
		case CS40L50_MBOX_LE_EST_START:
			dev_dbg(cs40l50->dev, "Mailbox: LE_EST_START\n");
			break;
		case CS40L50_MBOX_LE_EST_DONE:
			dev_dbg(cs40l50->dev, "Mailbox: LE_EST_DONE\n");
			break;
		case CS40L50_MBOX_AWAKE:
			dev_dbg(cs40l50->dev, "Mailbox: AWAKE\n");
			break;
		case CS40L50_MBOX_INIT:
			dev_dbg(cs40l50->dev, "Mailbox: INIT\n");
			break;
		case CS40L50_MBOX_ACK:
			dev_dbg(cs40l50->dev, "Mailbox: ACK\n");
			break;
		case CS40L50_MBOX_ERR_EVENT_UNMAPPED:
			dev_err(cs40l50->dev, "Unmapped event\n");
			break;
		case CS40L50_MBOX_ERR_EVENT_MODIFY:
			dev_err(cs40l50->dev, "Failed to modify event index\n");
			break;
		case CS40L50_MBOX_ERR_NULLPTR:
			dev_err(cs40l50->dev, "Null pointer\n");
			break;
		case CS40L50_MBOX_ERR_BRAKING:
			dev_err(cs40l50->dev, "Braking not in progress\n");
			break;
		case CS40L50_MBOX_ERR_INVAL_SRC:
			dev_err(cs40l50->dev, "Suspend/resume invalid source\n");
			break;
		case CS40L50_MBOX_ERR_ENABLE_RANGE:
			dev_err(cs40l50->dev, "GPIO enable out of range\n");
			break;
		case CS40L50_MBOX_ERR_GPIO_UNMAPPED:
			dev_err(cs40l50->dev, "GPIO not mapped\n");
			break;
		case CS40L50_MBOX_ERR_ISR_RANGE:
			dev_err(cs40l50->dev, "GPIO ISR out of range\n");
			break;
		case CS40L50_MBOX_PERMANENT_SHORT:
			dev_crit(cs40l50->dev, "Permanent short detected\n");
			break;
		case CS40L50_MBOX_RUNTIME_SHORT:
			dev_err(cs40l50->dev, "Runtime short detected\n");
			error = cs40l50_error_release(cs40l50);
			if (error)
				goto out_mutex;
			break;
		default:
			dev_err(cs40l50->dev, "Payload %#X not recognized\n", val);
			error = -EINVAL;
			goto out_mutex;
		}
	}

	error = -EIO;

out_mutex:
	mutex_unlock(&cs40l50->lock);

	return IRQ_RETVAL(!error);
}

static irqreturn_t cs40l50_error(int irq, void *data);

static const struct cs40l50_irq cs40l50_irqs[] = {
	CS40L50_IRQ(AMP_SHORT,		"Amp short",		error),
	CS40L50_IRQ(VIRT2_MBOX,		"Mailbox",		process_mbox),
	CS40L50_IRQ(TEMP_ERR,		"Overtemperature",	error),
	CS40L50_IRQ(BST_UVP,		"Boost undervoltage",	error),
	CS40L50_IRQ(BST_SHORT,		"Boost short",		error),
	CS40L50_IRQ(BST_ILIMIT,		"Boost current limit",	error),
	CS40L50_IRQ(UVLO_VDDBATT,	"Boost UVLO",		error),
	CS40L50_IRQ(GLOBAL_ERROR,	"Global",		error),
};

static irqreturn_t cs40l50_error(int irq, void *data)
{
	struct cs40l50_private *cs40l50 = data;

	dev_err(cs40l50->dev, "%s error\n", cs40l50_irqs[irq].name);

	return IRQ_RETVAL(!cs40l50_error_release(cs40l50));
}

static const struct regmap_irq cs40l50_reg_irqs[] = {
	CS40L50_REG_IRQ(IRQ1_INT_1,	AMP_SHORT),
	CS40L50_REG_IRQ(IRQ1_INT_2,	VIRT2_MBOX),
	CS40L50_REG_IRQ(IRQ1_INT_8,	TEMP_ERR),
	CS40L50_REG_IRQ(IRQ1_INT_9,	BST_UVP),
	CS40L50_REG_IRQ(IRQ1_INT_9,	BST_SHORT),
	CS40L50_REG_IRQ(IRQ1_INT_9,	BST_ILIMIT),
	CS40L50_REG_IRQ(IRQ1_INT_10,	UVLO_VDDBATT),
	CS40L50_REG_IRQ(IRQ1_INT_18,	GLOBAL_ERROR),
};

static struct regmap_irq_chip cs40l50_irq_chip = {
	.name =			"CS40L50 IRQ Controller",

	.status_base =		CS40L50_IRQ1_INT_1,
	.mask_base =		CS40L50_IRQ1_MASK_1,
	.ack_base =		CS40L50_IRQ1_INT_1,
	.num_regs =		22,

	.irqs =			cs40l50_reg_irqs,
	.num_irqs =		ARRAY_SIZE(cs40l50_reg_irqs),

	.runtime_pm =		true,
};

static int cs40l50_irq_init(struct cs40l50_private *cs40l50)
{
	struct device *dev = cs40l50->dev;
	int error, i, irq;

	error = devm_regmap_add_irq_chip(dev, cs40l50->regmap, cs40l50->irq,
					 IRQF_ONESHOT | IRQF_SHARED, 0,
					 &cs40l50_irq_chip, &cs40l50->irq_data);
	if (error)
		return error;

	for (i = 0; i < ARRAY_SIZE(cs40l50_irqs); i++) {
		irq = regmap_irq_get_virq(cs40l50->irq_data, cs40l50_irqs[i].irq);
		if (irq < 0) {
			dev_err(dev, "Failed getting %s\n", cs40l50_irqs[i].name);
			return irq;
		}

		error = devm_request_threaded_irq(dev, irq, NULL,
						  cs40l50_irqs[i].handler,
						  IRQF_ONESHOT | IRQF_SHARED,
						  cs40l50_irqs[i].name, cs40l50);
		if (error) {
			dev_err(dev, "Failed requesting %s\n", cs40l50_irqs[i].name);
			return error;
		}
	}

	return 0;
}

static int cs40l50_part_num_resolve(struct cs40l50_private *cs40l50)
{
	struct device *dev = cs40l50->dev;
	int error;

	error = regmap_read(cs40l50->regmap, CS40L50_DEVID, &cs40l50->devid);
	if (error)
		return error;

	if (cs40l50->devid != CS40L50_DEVID_A) {
		dev_err(dev, "Invalid device ID: %#010X\n", cs40l50->devid);
		return -EINVAL;
	}

	error = regmap_read(cs40l50->regmap, CS40L50_REVID, &cs40l50->revid);
	if (error)
		return error;

	if (cs40l50->revid != CS40L50_REVID_B0) {
		dev_err(dev, "Invalid revision: %#04X\n", cs40l50->revid);
		return -EINVAL;
	}

	dev_info(dev, "Cirrus Logic CS40L50 revision %02X\n", cs40l50->revid);

	return 0;
}

int cs40l50_probe(struct cs40l50_private *cs40l50)
{
	struct device *dev = cs40l50->dev;
	int error;

	mutex_init(&cs40l50->lock);

	cs40l50->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(cs40l50->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(cs40l50->reset_gpio),
				     "Failed getting reset GPIO\n");

	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(cs40l50_supplies),
					cs40l50_supplies);
	if (error)
		goto err_reset;

	error = regulator_bulk_enable(ARRAY_SIZE(cs40l50_supplies),
				      cs40l50_supplies);
	if (error)
		goto err_reset;

	usleep_range(CS40L50_CP_READY_US, CS40L50_CP_READY_US + 100);

	gpiod_set_value_cansleep(cs40l50->reset_gpio, 1);

	usleep_range(CS40L50_CP_READY_US, CS40L50_CP_READY_US + 1000);

	pm_runtime_set_autosuspend_delay(cs40l50->dev, CS40L50_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(cs40l50->dev);
	pm_runtime_set_active(cs40l50->dev);
	pm_runtime_get_noresume(cs40l50->dev);
	devm_pm_runtime_enable(cs40l50->dev);

	error = cs40l50_part_num_resolve(cs40l50);
	if (error)
		goto err_supplies;

	error = cs40l50_irq_init(cs40l50);
	if (error)
		goto err_supplies;

	error = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE, cs40l50_devs,
				     ARRAY_SIZE(cs40l50_devs), NULL, 0, NULL);
	if (error)
		goto err_supplies;

	pm_runtime_mark_last_busy(cs40l50->dev);
	pm_runtime_put_autosuspend(cs40l50->dev);

	return 0;

err_supplies:
	regulator_bulk_disable(ARRAY_SIZE(cs40l50_supplies), cs40l50_supplies);
err_reset:
	gpiod_set_value_cansleep(cs40l50->reset_gpio, 0);

	return error;
}
EXPORT_SYMBOL_GPL(cs40l50_probe);

int cs40l50_remove(struct cs40l50_private *cs40l50)
{
	regulator_bulk_disable(ARRAY_SIZE(cs40l50_supplies), cs40l50_supplies);
	gpiod_set_value_cansleep(cs40l50->reset_gpio, 1);

	return 0;
}
EXPORT_SYMBOL_GPL(cs40l50_remove);

static int cs40l50_runtime_suspend(struct device *dev)
{
	struct cs40l50_private *cs40l50 = dev_get_drvdata(dev);

	return regmap_write(cs40l50->regmap, CS40L50_DSP_MBOX, CS40L50_ALLOW_HIBER);
}

static int cs40l50_runtime_resume(struct device *dev)
{
	struct cs40l50_private *cs40l50 = dev_get_drvdata(dev);
	int error, i;
	u32 val;

	/* Device NAKs when exiting hibernation, so optionally retry here. */
	for (i = 0; i < CS40L50_DSP_TIMEOUT_COUNT; i++) {
		error = regmap_write(cs40l50->regmap, CS40L50_DSP_MBOX,
				     CS40L50_PREVENT_HIBER);
		if (!error)
			break;

		usleep_range(CS40L50_DSP_POLL_US, CS40L50_DSP_POLL_US + 100);
	}

	for (; i < CS40L50_DSP_TIMEOUT_COUNT; i++) {
		error = regmap_read(cs40l50->regmap, CS40L50_DSP_MBOX, &val);
		if (!error && val == 0)
			return 0;

		usleep_range(CS40L50_DSP_POLL_US, CS40L50_DSP_POLL_US + 100);
	}

	return error ? error : -ETIMEDOUT;
}

EXPORT_GPL_DEV_PM_OPS(cs40l50_pm_ops) = {
	RUNTIME_PM_OPS(cs40l50_runtime_suspend, cs40l50_runtime_resume, NULL)
};

MODULE_DESCRIPTION("CS40L50 Advanced Haptic Driver");
MODULE_AUTHOR("James Ogletree, Cirrus Logic Inc. <james.ogletree@cirrus.com>");
MODULE_LICENSE("GPL");
