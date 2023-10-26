// SPDX-License-Identifier: GPL-2.0
/*
 * PFSM (Pre-configurable Finite State Machine) driver for TI TPS65224 PMIC
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Based on the TPS6594 Pre-configurable Finite State Machine Driver by
 * Julien Panis <jpanis@baylibre.com>
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/mfd/tps65224.h>

#include <linux/tps65224_pfsm.h>

#define TPS65224_STARTUP_DEST_MCU_ONLY_VAL 2
#define TPS65224_STARTUP_DEST_ACTIVE_VAL   3
#define TPS65224_STARTUP_DEST_SHIFT	  5
#define TPS65224_STARTUP_DEST_MCU_ONLY	  (TPS65224_STARTUP_DEST_MCU_ONLY_VAL \
					   << TPS65224_STARTUP_DEST_SHIFT)
#define TPS65224_STARTUP_DEST_ACTIVE	  (TPS65224_STARTUP_DEST_ACTIVE_VAL \
					   << TPS65224_STARTUP_DEST_SHIFT)

/*
 * To update the PMIC firmware, the user must be able to access
 * page 0 (user registers) and page 1 (NVM control and configuration).
 */
#define TPS65224_PMIC_MAX_POS 0x200

#define TPS65224_FILE_TO_PFSM(f) container_of((f)->private_data, struct tps65224_pfsm, miscdev)

/**
 * struct tps65224_pfsm - device private data structure
 *
 * @miscdev: misc device infos
 * @regmap:  regmap for accessing the device registers
 */
struct tps65224_pfsm {
	struct miscdevice miscdev;
	struct regmap *regmap;
};

static ssize_t tps65224_pfsm_read(struct file *f, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct tps65224_pfsm *pfsm = TPS65224_FILE_TO_PFSM(f);
	loff_t pos = *ppos;
	unsigned int val;
	int ret;
	int i;

	if (pos < 0)
		return -EINVAL;
	if (pos >= TPS65224_PMIC_MAX_POS)
		return 0;
	if (count > TPS65224_PMIC_MAX_POS - pos)
		count = TPS65224_PMIC_MAX_POS - pos;

	for (i = 0 ; i < count ; i++) {
		ret = regmap_read(pfsm->regmap, pos + i, &val);
		if (ret)
			return ret;

		if (put_user(val, buf + i))
			return -EFAULT;
	}

	*ppos = pos + count;

	return count;
}

static ssize_t tps65224_pfsm_write(struct file *f, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct tps65224_pfsm *pfsm = TPS65224_FILE_TO_PFSM(f);
	loff_t pos = *ppos;
	char val;
	int ret;
	int i;

	if (pos < 0)
		return -EINVAL;
	if (pos >= TPS65224_PMIC_MAX_POS || !count)
		return 0;
	if (count > TPS65224_PMIC_MAX_POS - pos)
		count = TPS65224_PMIC_MAX_POS - pos;

	for (i = 0 ; i < count ; i++) {
		if (get_user(val, buf + i))
			return -EFAULT;

		ret = regmap_write(pfsm->regmap, pos + i, val);
		if (ret)
			return ret;
	}

	*ppos = pos + count;

	return count;
}

static int tps65224_pfsm_configure_ret_trig(struct regmap *regmap, u8 gpio_ret, u8 ddr_ret)
{
	int ret;

	if (gpio_ret)
		ret = regmap_set_bits(regmap, TPS65224_REG_FSM_I2C_TRIGGERS,
					TPS65224_BIT_TRIGGER_I2C(5) | TPS65224_BIT_TRIGGER_I2C(6));
	else
		ret = regmap_clear_bits(regmap, TPS65224_REG_FSM_I2C_TRIGGERS,
					TPS65224_BIT_TRIGGER_I2C(5) | TPS65224_BIT_TRIGGER_I2C(6));
	if (ret)
		return ret;

	if (ddr_ret)
		ret = regmap_set_bits(regmap, TPS65224_REG_FSM_I2C_TRIGGERS,
					  TPS65224_BIT_TRIGGER_I2C(7));
	else
		ret = regmap_clear_bits(regmap, TPS65224_REG_FSM_I2C_TRIGGERS,
					TPS65224_BIT_TRIGGER_I2C(7));

	return ret;
}

static long tps65224_pfsm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct tps65224_pfsm *pfsm = TPS65224_FILE_TO_PFSM(f);
	struct pmic_state_opt state_opt;
	void __user *argp = (void __user *)arg;
	int ret = -ENOIOCTLCMD;

	switch (cmd) {
	case PMIC_GOTO_STANDBY:
		/* Force trigger */
		ret = regmap_write_bits(pfsm->regmap, TPS65224_REG_FSM_I2C_TRIGGERS,
					TPS65224_BIT_TRIGGER_I2C(0), TPS65224_BIT_TRIGGER_I2C(0));
		break;
	case PMIC_UPDATE_PGM:
		/* Force trigger */
		ret = regmap_write_bits(pfsm->regmap, TPS65224_REG_FSM_I2C_TRIGGERS,
					TPS65224_BIT_TRIGGER_I2C(3), TPS65224_BIT_TRIGGER_I2C(3));
		break;
	case PMIC_SET_ACTIVE_STATE:
		/* Modify NSLEEP1-2 bits */
		ret = regmap_set_bits(pfsm->regmap, TPS65224_REG_FSM_NSLEEP_TRIGGERS,
					  TPS65224_BIT_NSLEEP1B | TPS65224_BIT_NSLEEP2B);
		break;
	case PMIC_SET_MCU_ONLY_STATE:
		if (copy_from_user(&state_opt, argp, sizeof(state_opt)))
			return -EFAULT;

		/* Configure retention triggers */
		ret = tps65224_pfsm_configure_ret_trig(pfsm->regmap, state_opt.gpio_retention,
							  state_opt.ddr_retention);
		if (ret)
			return ret;

		/* Modify NSLEEP1-2 bits */
		ret = regmap_clear_bits(pfsm->regmap, TPS65224_REG_FSM_NSLEEP_TRIGGERS,
					TPS65224_BIT_NSLEEP1B);
		if (ret)
			return ret;

		ret = regmap_set_bits(pfsm->regmap, TPS65224_REG_FSM_NSLEEP_TRIGGERS,
					  TPS65224_BIT_NSLEEP2B);
		break;
	case PMIC_SET_RETENTION_STATE:
		if (copy_from_user(&state_opt, argp, sizeof(state_opt)))
			return -EFAULT;

		/* Configure wake-up destination */
		if (state_opt.mcu_only_startup_dest)
			ret = regmap_write_bits(pfsm->regmap, TPS65224_REG_CONFIG_2,
						TPS65224_BIT_STARTUP_INT,
						TPS65224_STARTUP_DEST_MCU_ONLY);
		else
			ret = regmap_write_bits(pfsm->regmap, TPS65224_REG_CONFIG_2,
						TPS65224_BIT_STARTUP_INT,
						TPS65224_STARTUP_DEST_ACTIVE);
		if (ret)
			return ret;

		/* Configure retention triggers */
		ret = tps65224_pfsm_configure_ret_trig(pfsm->regmap, state_opt.gpio_retention,
							  state_opt.ddr_retention);
		if (ret)
			return ret;

		/* Modify NSLEEP1-2 bits */
		ret = regmap_clear_bits(pfsm->regmap, TPS65224_REG_FSM_NSLEEP_TRIGGERS,
					TPS65224_BIT_NSLEEP2B);
		break;
	}

	return ret;
}

static const struct file_operations tps65224_pfsm_fops = {
	.owner		= THIS_MODULE,
	.llseek		= generic_file_llseek,
	.read		= tps65224_pfsm_read,
	.write		= tps65224_pfsm_write,
	.unlocked_ioctl	= tps65224_pfsm_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static irqreturn_t tps65224_pfsm_isr(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	int i;

	for (i = 0 ; i < pdev->num_resources ; i++) {
		if (irq == platform_get_irq_byname(pdev, pdev->resource[i].name)) {
			dev_info(pdev->dev.parent, "%s event detected\n", pdev->resource[i].name);
			return IRQ_HANDLED;
		}
	}

	return IRQ_NONE;
}

static int tps65224_pfsm_probe(struct platform_device *pdev)
{
	struct tps65224_pfsm *pfsm;
	struct tps65224 *tps = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	int irq;
	int ret;
	int i;

	pfsm = devm_kzalloc(dev, sizeof(struct tps65224_pfsm), GFP_KERNEL);
	if (!pfsm)
		return -ENOMEM;

	pfsm->regmap = tps->regmap;

	pfsm->miscdev.minor = MISC_DYNAMIC_MINOR;
	pfsm->miscdev.name = devm_kasprintf(dev, GFP_KERNEL, "pfsm-%ld-0x%02x",
						tps->chip_id, tps->reg);
	pfsm->miscdev.fops = &tps65224_pfsm_fops;
	pfsm->miscdev.parent = dev->parent;

	for (i = 0 ; i < pdev->num_resources ; i++) {
		irq = platform_get_irq_byname(pdev, pdev->resource[i].name);
		if (irq < 0)
			return dev_err_probe(dev, irq, "Failed to get %s irq\n",
						 pdev->resource[i].name);

		ret = devm_request_threaded_irq(dev, irq, NULL,
						tps65224_pfsm_isr, IRQF_ONESHOT,
						pdev->resource[i].name, pdev);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to request irq\n");
	}

	platform_set_drvdata(pdev, pfsm);

	return misc_register(&pfsm->miscdev);
}

static void tps65224_pfsm_remove(struct platform_device *pdev)
{
	struct tps65224_pfsm *pfsm = platform_get_drvdata(pdev);

	misc_deregister(&pfsm->miscdev);
}

static struct platform_driver tps65224_pfsm_driver = {
	.driver	= {
		.name = "tps65224-pfsm",
	},
	.probe = tps65224_pfsm_probe,
	.remove_new = tps65224_pfsm_remove,
};

module_platform_driver(tps65224_pfsm_driver);

MODULE_ALIAS("platform:tps65224-pfsm");
MODULE_AUTHOR("Gairuboina Sirisha <sirisha.gairuboina@ltts.com>");
MODULE_DESCRIPTION("TPS65224 Pre-configurable Finite State Machine Driver");
MODULE_LICENSE("GPL");
