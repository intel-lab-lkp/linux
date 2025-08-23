/*
 * Copyright (C) 2011 Philippe Rétornaz
 *
 * Based on twl4030-pwrbutton driver by:
 *     Peter De Schrijver <peter.de-schrijver@nokia.com>
 *     Felipe Balbi <felipe.balbi@nokia.com>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335  USA
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mfd/mc13783.h>
#include <linux/property.h>
#include <linux/sched.h>
#include <linux/slab.h>

struct mc13xxx_button_devtype {
	int button_id_max;
};

struct mc13783_pwrb {
	struct input_dev *pwr;
	struct mc13xxx *mc13783;
	int flags;
	unsigned short keymap[3];
};

#define MC13783_PWRB_B1_POL_INVERT	(1 << 0)
#define MC13783_PWRB_B2_POL_INVERT	(1 << 1)
#define MC13783_PWRB_B3_POL_INVERT	(1 << 2)

#define MC13783_REG_INTERRUPT_SENSE_1		5
#define MC13783_IRQSENSE1_ONOFD1S		(1 << 3)
#define MC13783_IRQSENSE1_ONOFD2S		(1 << 4)
#define MC13783_IRQSENSE1_ONOFD3S		(1 << 5)

#define MC13783_REG_POWER_CONTROL_2		15
#define MC13783_POWER_CONTROL_2_ON1BDBNC	4
#define MC13783_POWER_CONTROL_2_ON2BDBNC	6
#define MC13783_POWER_CONTROL_2_ON3BDBNC	8
#define MC13783_POWER_CONTROL_2_ON1BRSTEN	(1 << 1)
#define MC13783_POWER_CONTROL_2_ON2BRSTEN	(1 << 2)
#define MC13783_POWER_CONTROL_2_ON3BRSTEN	(1 << 3)

static irqreturn_t button_irq(int irq, void *_priv)
{
	struct mc13783_pwrb *priv = _priv;
	int val;

	mc13xxx_irq_ack(priv->mc13783, irq);
	mc13xxx_reg_read(priv->mc13783, MC13783_REG_INTERRUPT_SENSE_1, &val);

	switch (irq) {
	case MC13783_IRQ_ONOFD1:
		val = val & MC13783_IRQSENSE1_ONOFD1S ? 1 : 0;
		if (priv->flags & MC13783_PWRB_B1_POL_INVERT)
			val ^= 1;
		input_report_key(priv->pwr, priv->keymap[0], val);
		break;

	case MC13783_IRQ_ONOFD2:
		val = val & MC13783_IRQSENSE1_ONOFD2S ? 1 : 0;
		if (priv->flags & MC13783_PWRB_B2_POL_INVERT)
			val ^= 1;
		input_report_key(priv->pwr, priv->keymap[1], val);
		break;

	case MC13783_IRQ_ONOFD3:
		val = val & MC13783_IRQSENSE1_ONOFD3S ? 1 : 0;
		if (priv->flags & MC13783_PWRB_B3_POL_INVERT)
			val ^= 1;
		input_report_key(priv->pwr, priv->keymap[2], val);
		break;
	}

	input_sync(priv->pwr);

	return IRQ_HANDLED;
}

static irqreturn_t button1_irq(int irq, void *_priv)
{
	return button_irq(MC13783_IRQ_ONOFD1, _priv);
}

static irqreturn_t button2_irq(int irq, void *_priv)
{
	return button_irq(MC13783_IRQ_ONOFD2, _priv);
}

static irqreturn_t button3_irq(int irq, void *_priv)
{
	return button_irq(MC13783_IRQ_ONOFD3, _priv);
}

#ifdef CONFIG_OF
static struct mc13xxx_buttons_platform_data __init *mc13xxx_pwrbutton_probe_dt(
	struct platform_device *pdev)
{
	struct mc13xxx_buttons_platform_data *pdata;
	struct fwnode_handle *child;
	struct device *dev = &pdev->dev;
	struct mc13xxx_button_devtype *devtype =
		(struct mc13xxx_button_devtype *)platform_get_device_id(pdev)->driver_data;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	struct fwnode_handle *parent __free(fwnode_handle) =
		device_get_named_child_node(dev->parent, "buttons");
	if (!parent)
		return ERR_PTR(-ENODATA);

	fwnode_for_each_child_node(parent, child) {
		u32 idx;
		u8 dbnc = MC13783_BUTTON_DBNC_30MS;
		u16 dbnc_ms;

		if (fwnode_property_read_u32(child, "reg", &idx))
			continue;

		if (idx > devtype->button_id_max) {
			dev_warn(dev, "reg out of range\n");
			continue;
		}

		fwnode_property_read_u16(child, "debounce-delay-ms", &dbnc_ms);
		switch (dbnc_ms) {
		case 0:
			dbnc = MC13783_BUTTON_DBNC_0MS;
			break;
		case 30:
			dbnc = MC13783_BUTTON_DBNC_30MS;
			break;
		case 150:
			dbnc = MC13783_BUTTON_DBNC_150MS;
			break;
		case 750:
			dbnc = MC13783_BUTTON_DBNC_750MS;
			break;
		default:
			dev_warn(dev, "invalid debounce-delay-ms value\n");
			continue;
		}

		if (fwnode_property_read_u32(child, "linux,code", &pdata->b_on_key[idx]))
			continue;

		if (fwnode_property_read_bool(child, "active-low"))
			pdata->b_on_flags[idx] |= MC13783_BUTTON_POL_INVERT;

		if (fwnode_property_read_bool(child, "fsl,enable-reset"))
			pdata->b_on_flags[idx] |= MC13783_BUTTON_RESET_EN;

		pdata->b_on_flags[idx] |= MC13783_BUTTON_ENABLE | dbnc;
	}

	return pdata;
}
#else
static inline struct mc13xxx_buttons_platform_data __init *mc13xxx_pwrbutton_probe_dt(
	struct platform_device *pdev)
{
	return ERR_PTR(-ENODEV);
}
#endif

static int __init mc13783_pwrbutton_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct mc13xxx_buttons_platform_data *pdata;
	struct mc13xxx *mc13783 = dev_get_drvdata(pdev->dev.parent);
	struct mc13xxx_button_devtype *devtype =
		(struct mc13xxx_button_devtype *)pdev->id_entry->driver_data;
	struct input_dev *pwr;
	struct mc13783_pwrb *priv;
	int err = 0;
	int reg = 0;

	pdata = dev_get_platdata(&pdev->dev);
	if (dev->parent->of_node) {
		pdata = mc13xxx_pwrbutton_probe_dt(pdev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	} else if (!pdata) {
		dev_err(dev, "missing platform data\n");
		return -ENODATA;
	}

	pwr = devm_input_allocate_device(&pdev->dev);
	if (!pwr)
		return -ENOMEM;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (devtype->button_id_max < 2 && pdata->b_on_flags[2] & 0x3) {
		dev_err(&pdev->dev, "button not supported\n");
		return -ENODEV;
	}

	reg |= (pdata->b_on_flags[0] & 0x3) << MC13783_POWER_CONTROL_2_ON1BDBNC;
	reg |= (pdata->b_on_flags[1] & 0x3) << MC13783_POWER_CONTROL_2_ON2BDBNC;
	reg |= (pdata->b_on_flags[2] & 0x3) << MC13783_POWER_CONTROL_2_ON3BDBNC;

	priv->pwr = pwr;
	priv->mc13783 = mc13783;

	mc13xxx_lock(mc13783);

	if (pdata->b_on_flags[0] & MC13783_BUTTON_ENABLE) {
		priv->keymap[0] = pdata->b_on_key[0];
		if (pdata->b_on_key[0] != KEY_RESERVED)
			__set_bit(pdata->b_on_key[0], pwr->keybit);

		if (pdata->b_on_flags[0] & MC13783_BUTTON_POL_INVERT)
			priv->flags |= MC13783_PWRB_B1_POL_INVERT;

		if (pdata->b_on_flags[0] & MC13783_BUTTON_RESET_EN)
			reg |= MC13783_POWER_CONTROL_2_ON1BRSTEN;

		err = mc13xxx_irq_request(mc13783, MC13783_IRQ_ONOFD1,
					  button1_irq, "b1on", priv);
		if (err) {
			dev_dbg(&pdev->dev, "Can't request irq\n");
			goto free_mc13xxx_lock;
		}
	}

	if (pdata->b_on_flags[1] & MC13783_BUTTON_ENABLE) {
		priv->keymap[1] = pdata->b_on_key[1];
		if (pdata->b_on_key[1] != KEY_RESERVED)
			__set_bit(pdata->b_on_key[1], pwr->keybit);

		if (pdata->b_on_flags[1] & MC13783_BUTTON_POL_INVERT)
			priv->flags |= MC13783_PWRB_B2_POL_INVERT;

		if (pdata->b_on_flags[1] & MC13783_BUTTON_RESET_EN)
			reg |= MC13783_POWER_CONTROL_2_ON2BRSTEN;

		err = mc13xxx_irq_request(mc13783, MC13783_IRQ_ONOFD2,
					  button2_irq, "b2on", priv);
		if (err) {
			dev_dbg(&pdev->dev, "Can't request irq\n");
			goto free_irq_b1;
		}
	}

	if (pdata->b_on_flags[2] & MC13783_BUTTON_ENABLE) {
		priv->keymap[2] = pdata->b_on_key[2];
		if (pdata->b_on_key[2] != KEY_RESERVED)
			__set_bit(pdata->b_on_key[2], pwr->keybit);

		if (pdata->b_on_flags[2] & MC13783_BUTTON_POL_INVERT)
			priv->flags |= MC13783_PWRB_B3_POL_INVERT;

		if (pdata->b_on_flags[2] & MC13783_BUTTON_RESET_EN)
			reg |= MC13783_POWER_CONTROL_2_ON3BRSTEN;

		err = mc13xxx_irq_request(mc13783, MC13783_IRQ_ONOFD3,
					  button3_irq, "b3on", priv);
		if (err) {
			dev_dbg(&pdev->dev, "Can't request irq: %d\n", err);
			goto free_irq_b2;
		}
	}

	mc13xxx_reg_rmw(mc13783, MC13783_REG_POWER_CONTROL_2, 0x3FE, reg);

	mc13xxx_unlock(mc13783);

	pwr->name = "mc13783_pwrbutton";
	pwr->phys = "mc13783_pwrbutton/input0";

	pwr->keycode = priv->keymap;
	pwr->keycodemax = ARRAY_SIZE(priv->keymap);
	pwr->keycodesize = sizeof(priv->keymap[0]);
	__set_bit(EV_KEY, pwr->evbit);

	err = input_register_device(pwr);
	if (err) {
		dev_dbg(&pdev->dev, "Can't register power button: %d\n", err);
		goto free_irq;
	}

	platform_set_drvdata(pdev, priv);

	return 0;

free_irq:
	mc13xxx_lock(mc13783);

	if (pdata->b_on_flags[2] & MC13783_BUTTON_ENABLE)
		mc13xxx_irq_free(mc13783, MC13783_IRQ_ONOFD3, priv);

free_irq_b2:
	if (pdata->b_on_flags[1] & MC13783_BUTTON_ENABLE)
		mc13xxx_irq_free(mc13783, MC13783_IRQ_ONOFD2, priv);

free_irq_b1:
	if (pdata->b_on_flags[0] & MC13783_BUTTON_ENABLE)
		mc13xxx_irq_free(mc13783, MC13783_IRQ_ONOFD1, priv);

free_mc13xxx_lock:
	mc13xxx_unlock(mc13783);

	return err;
}

static void mc13783_pwrbutton_remove(struct platform_device *pdev)
{
	struct mc13783_pwrb *priv = platform_get_drvdata(pdev);
	const struct mc13xxx_buttons_platform_data *pdata;
	struct mc13xxx_button_devtype *devtype =
		(struct mc13xxx_button_devtype *)pdev->id_entry->driver_data;

	pdata = dev_get_platdata(&pdev->dev);

	mc13xxx_lock(priv->mc13783);

	if (devtype->button_id_max >= 2 &&
		pdata->b_on_flags[2] & MC13783_BUTTON_ENABLE)
		mc13xxx_irq_free(priv->mc13783, MC13783_IRQ_ONOFD3, priv);
	if (pdata->b_on_flags[1] & MC13783_BUTTON_ENABLE)
		mc13xxx_irq_free(priv->mc13783, MC13783_IRQ_ONOFD2, priv);
	if (pdata->b_on_flags[0] & MC13783_BUTTON_ENABLE)
		mc13xxx_irq_free(priv->mc13783, MC13783_IRQ_ONOFD1, priv);

	mc13xxx_unlock(priv->mc13783);
}

static const struct mc13xxx_button_devtype mc13783_button_devtype = {
	.button_id_max	= 2,
};

static const struct mc13xxx_button_devtype mc13892_button_devtype = {
	/* PWRON3 is not supported yet. */
	.button_id_max	= 1,
};

static const struct mc13xxx_button_devtype mc34708_button_devtype = {
	.button_id_max	= 1,
};

static const struct platform_device_id mc13xxx_pwrbutton_idtable[] = {
	{ "mc13783-pwrbutton", (kernel_ulong_t)&mc13783_button_devtype },
	{ "mc13892-pwrbutton", (kernel_ulong_t)&mc13892_button_devtype },
	{ "mc34708-pwrbutton", (kernel_ulong_t)&mc34708_button_devtype },
	{ /* sentinel */ }
};

static struct platform_driver mc13783_pwrbutton_driver = {
	.driver		= {
		.name	= "mc13783-pwrbutton",
	},
	.id_table	= mc13xxx_pwrbutton_idtable,
	.remove		= mc13783_pwrbutton_remove,
};

module_platform_driver_probe(mc13783_pwrbutton_driver, mc13783_pwrbutton_probe);

MODULE_ALIAS("platform:mc13783-pwrbutton");
MODULE_DESCRIPTION("MC13783 Power Button");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Philippe Retornaz");
