// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Spreadtrum PMIC USB Type-C
 *
 * Copyright (C) 2023 Unisoc Inc.
 */
#include <linux/extcon.h>
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/usb/tcpm.h>
#include <linux/usb/typec.h>

/* registers definitions for controller REGS_TYPEC */
#define SC27XX_EN			0x00
#define SC27XX_MODE			0x04
#define SC27XX_INT_EN			0x0c
#define SC27XX_INT_CLR			0x10
#define SC27XX_INT_MASK			0x18
#define SC27XX_STATUS			0x1c
#define SC27XX_TCCDE_CNT		0x20
#define SC27XX_RTRIM			0x3c

/* SC27XX_TYPEC_EN */
#define SC27XX_TYPEC_USB20_ONLY		BIT(4)

/* SC27XX_TYPEC MODE */
#define SC27XX_MODE_SNK			0
#define SC27XX_MODE_SRC			1
#define SC27XX_MODE_DRP			2
#define SC27XX_MODE_MASK		3

/* SC27XX_INT_EN */
#define SC27XX_ATTACH_INT_EN		BIT(0)
#define SC27XX_DETACH_INT_EN		BIT(1)

/* SC27XX_INT_MASK */
#define SC27XX_ATTACH_INT		BIT(0)
#define SC27XX_DETACH_INT		BIT(1)

#define SC27XX_STATE_MASK		GENMASK(4, 0)
#define SC27XX_EVENT_MASK		GENMASK(9, 0)

#define SC2730_EFUSE_CC1_SHIFT		5
#define SC2730_EFUSE_CC2_SHIFT		0

#define SC27XX_CC1_MASK(n)		GENMASK((n) + 4, (n))
#define SC27XX_CC2_MASK(n)		GENMASK((n) + 4, (n))
#define SC27XX_CC_SHIFT(n)		(n)

/* modify sc2730 tcc debunce */
#define SC27XX_TCC_DEBOUNCE_CNT		0xc7f

/* pmic name number*/
#define SC2730				0x01

/* CC DETECT */
#define SC2730_CC1_CHECK		BIT(5)

enum sprd_pmic_typec_connection_state {
	TYPEC_DETACHED_SNK,
	TYPEC_ATTACHWAIT_SNK,
	TYPEC_ATTACHED_SNK,
	TYPEC_DETACHED_SRC,
	TYPEC_ATTACHWAIT_SRC,
	TYPEC_ATTACHED_SRC,
	TYPEC_POWERED_CABLE,
	TYPEC_AUDIO_CABLE,
	TYPEC_DEBUG_CABLE,
	TYPEC_TOGGLE_SLEEP,
	TYPEC_ERR_RECOV,
	TYPEC_DISABLED,
	TYPEC_TRY_SNK,
	TYPEC_TRY_WAIT_SRC,
	TYPEC_TRY_SRC,
	TYPEC_TRY_WAIT_SNK,
	TYPEC_UNSUPOORT_ACC,
	TYPEC_ORIENTED_DEBUG,
};

struct sprd_pmic_typec_variant_data {
	u8 pmic_number;
	u32 efuse_cc1_shift;
	u32 efuse_cc2_shift;
	u32 int_en;
	u32 int_clr;
	u32 mode;
	u32 attach_en;
	u32 detach_en;
	u32 state_mask;
	u32 event_mask;
};

static const struct sprd_pmic_typec_variant_data sc2730_data = {
	.pmic_number = SC2730,
	.efuse_cc1_shift = SC2730_EFUSE_CC1_SHIFT,
	.efuse_cc2_shift = SC2730_EFUSE_CC2_SHIFT,
	.int_en = SC27XX_INT_EN,
	.int_clr = SC27XX_INT_CLR,
	.mode = SC27XX_MODE,
	.attach_en = SC27XX_ATTACH_INT_EN,
	.detach_en = SC27XX_DETACH_INT_EN,
	.state_mask = SC27XX_STATE_MASK,
	.event_mask = SC27XX_EVENT_MASK,
};

struct sprd_pmic_typec {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
	int irq;
	int mode;
	struct extcon_dev *edev;
	bool usb20_only;
	bool partner_connected;

	u8 pd_swap_evt;
	spinlock_t lock;
	enum sprd_pmic_typec_connection_state state;
	enum sprd_pmic_typec_connection_state pre_state;
	enum typec_cc_polarity cc_polarity;
	struct typec_port *port;
	struct typec_partner *partner;
	struct typec_capability typec_cap;
	const struct sprd_pmic_typec_variant_data *var_data;
};

static const char * const sprd_pmic_typec_cc_polarity_roles[] = {
	[TYPEC_POLARITY_CC1] = "cc_1",
	[TYPEC_POLARITY_CC2] = "cc_2",
};

static void typec_set_cc_polarity_role(struct sprd_pmic_typec *sc,
				enum typec_cc_polarity polarity)
{
	if (sc->cc_polarity == polarity)
		return;

	sc->cc_polarity = polarity;
	sysfs_notify(&sc->dev->kobj, NULL, "typec_cc_polarity_role");
	kobject_uevent(&sc->dev->kobj, KOBJ_CHANGE);
}

static int sprd_pmic_typec_set_cc_polarity_role(struct sprd_pmic_typec *sc)
{
	enum typec_cc_polarity cc_polarity;
	u32 val;
	int ret;

	ret = regmap_read(sc->regmap, sc->base + SC27XX_STATUS, &val);
	if (ret < 0) {
		dev_err(sc->dev, "failed to read STATUS register.\n");
		return ret;
	}

	if (val & SC2730_CC1_CHECK)
		cc_polarity = TYPEC_POLARITY_CC1;
	else
		cc_polarity = TYPEC_POLARITY_CC2;

	typec_set_cc_polarity_role(sc, cc_polarity);

	return ret;
}

static int sprd_pmic_typec_connect(struct sprd_pmic_typec *sc, u32 status)
{
	enum typec_data_role data_role = TYPEC_DEVICE;
	enum typec_role power_role = TYPEC_SINK;
	enum typec_role vconn_role = TYPEC_SINK;
	struct typec_partner_desc desc;

	if (sc->partner)
		return 0;

	switch (sc->state) {
	case TYPEC_ATTACHED_SNK:
		power_role = TYPEC_SINK;
		data_role = TYPEC_DEVICE;
		vconn_role = TYPEC_SINK;
		break;
	case TYPEC_ATTACHED_SRC:
		power_role = TYPEC_SOURCE;
		data_role = TYPEC_HOST;
		vconn_role = TYPEC_SOURCE;
		break;
	}

	desc.usb_pd = 0;
	desc.identity = NULL;
	if (sc->state == TYPEC_AUDIO_CABLE)
		desc.accessory = TYPEC_ACCESSORY_AUDIO;
	else if (sc->state == TYPEC_DEBUG_CABLE)
		desc.accessory = TYPEC_ACCESSORY_DEBUG;
	else
		desc.accessory = TYPEC_ACCESSORY_NONE;

	sc->partner = typec_register_partner(sc->port, &desc);
	if (!sc->partner)
		return -ENODEV;

	typec_set_pwr_opmode(sc->port, TYPEC_PWR_MODE_USB);
	typec_set_pwr_role(sc->port, power_role);
	typec_set_data_role(sc->port, data_role);
	typec_set_vconn_role(sc->port, vconn_role);

	switch (sc->state) {
	case TYPEC_ATTACHED_SNK:
		sc->pre_state = TYPEC_ATTACHED_SNK;
		/*notify USB, USB PHY, charger, and bc1p2 driver*/
		extcon_set_state_sync(sc->edev, EXTCON_USB, true);
		break;
	case TYPEC_ATTACHED_SRC:
		sc->pre_state = TYPEC_ATTACHED_SRC;
		/*notify USB, USB PHY driver*/
		extcon_set_state_sync(sc->edev, EXTCON_USB_HOST, true);
		break;
	case TYPEC_AUDIO_CABLE:
		/*notify analog audio driver*/
		sc->pre_state = TYPEC_AUDIO_CABLE;
		extcon_set_state_sync(sc->edev, EXTCON_JACK_HEADPHONE, true);
		break;
	default:
		break;
	}

	spin_lock(&sc->lock);
	sc->partner_connected = true;
	spin_unlock(&sc->lock);
	sprd_pmic_typec_set_cc_polarity_role(sc);

	return 0;
}

static void sprd_pmic_typec_disconnect(struct sprd_pmic_typec *sc, u32 status)
{
	typec_unregister_partner(sc->partner);
	sc->partner = NULL;
	typec_set_pwr_opmode(sc->port, TYPEC_PWR_MODE_USB);
	typec_set_pwr_role(sc->port, TYPEC_SINK);
	typec_set_data_role(sc->port, TYPEC_DEVICE);
	typec_set_vconn_role(sc->port, TYPEC_SINK);

	spin_lock(&sc->lock);
	sc->partner_connected = false;
	spin_unlock(&sc->lock);

	switch (sc->pre_state) {
	case TYPEC_ATTACHED_SNK:
		extcon_set_state_sync(sc->edev, EXTCON_USB, false);
		break;
	case TYPEC_ATTACHED_SRC:
		extcon_set_state_sync(sc->edev, EXTCON_USB_HOST, false);
		break;
	case TYPEC_AUDIO_CABLE:
		extcon_set_state_sync(sc->edev, EXTCON_JACK_HEADPHONE, false);
		break;
	default:
		break;
	}
	sc->pre_state = TYPEC_DETACHED_SNK;
}

static irqreturn_t sprd_pmic_typec_interrupt(int irq, void *data)
{
	struct sprd_pmic_typec *sc = data;
	u32 event;
	int ret;

	dev_info(sc->dev, "%s enter line %d\n", __func__, __LINE__);
	ret = regmap_read(sc->regmap, sc->base + SC27XX_INT_MASK, &event);
	if (ret)
		return ret;

	event &= sc->var_data->event_mask;

	ret = regmap_read(sc->regmap, sc->base + SC27XX_STATUS, &sc->state);
	if (ret)
		goto clear_ints;

	sc->state &= sc->var_data->state_mask;

	if (event & SC27XX_ATTACH_INT) {
		ret = sprd_pmic_typec_connect(sc, sc->state);
		if (ret)
			dev_warn(sc->dev, "failed to register partner\n");
	} else if (event & SC27XX_DETACH_INT) {
		sprd_pmic_typec_disconnect(sc, sc->state);
	}

clear_ints:
	regmap_write(sc->regmap, sc->base + sc->var_data->int_clr, event);

	dev_info(sc->dev, "now works as DRP and is in %d state, event %d\n",
		sc->state, event);
	return IRQ_HANDLED;
}

static int sprd_pmic_typec_enable(struct sprd_pmic_typec *sc)
{
	int ret;
	u32 val;

	/* Set typec mode */
	ret = regmap_read(sc->regmap, sc->base + sc->var_data->mode, &val);
	if (ret)
		return ret;

	val &= ~SC27XX_MODE_MASK;
	switch (sc->mode) {
	case TYPEC_PORT_DFP:
		val |= SC27XX_MODE_SRC;
		break;
	case TYPEC_PORT_UFP:
		val |= SC27XX_MODE_SNK;
		break;
	case TYPEC_PORT_DRP:
		val |= SC27XX_MODE_DRP;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_write(sc->regmap, sc->base + sc->var_data->mode, val);
	if (ret)
		return ret;

	/* typec USB20 only flag, only work in snk mode */
	if (sc->typec_cap.data == TYPEC_PORT_UFP && sc->usb20_only) {
		ret = regmap_update_bits(sc->regmap, sc->base + SC27XX_EN,
					SC27XX_TYPEC_USB20_ONLY,
					SC27XX_TYPEC_USB20_ONLY);
		if (ret)
			return ret;
	}

	ret = regmap_write(sc->regmap, sc->base + SC27XX_TCCDE_CNT,
			SC27XX_TCC_DEBOUNCE_CNT);
	if (ret)
		return ret;

	/* Enable typec interrupt and enable typec */
	ret = regmap_read(sc->regmap, sc->base + sc->var_data->int_en, &val);
	if (ret)
		return ret;

	val |= sc->var_data->attach_en | sc->var_data->detach_en;
	return regmap_write(sc->regmap, sc->base + sc->var_data->int_en, val);
}

/* Use extcon mechanism for port management, notify USB, USB PHY, charger,
 * bc1p2, and analog audio drivers
 */
static const u32 sprd_pmic_typec_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_JACK_HEADPHONE,
	EXTCON_NONE,
};

static int sprd_pmic_typec_get_cc1_efuse(struct sprd_pmic_typec *sc)
{
	struct nvmem_cell *cell;
	u32 calib_data = 0;
	void *buf;
	size_t len;

	cell = nvmem_cell_get(sc->dev, "typec_cc1_cal");
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	memcpy(&calib_data, buf, min(len, sizeof(u32)));
	calib_data = (calib_data & SC27XX_CC1_MASK(sc->var_data->efuse_cc1_shift))
			>> SC27XX_CC_SHIFT(sc->var_data->efuse_cc1_shift);
	kfree(buf);

	return calib_data;
}

static int sprd_pmic_typec_get_cc2_efuse(struct sprd_pmic_typec *sc)
{
	struct nvmem_cell *cell;
	u32 calib_data = 0;
	void *buf;
	size_t len = 0;

	cell = nvmem_cell_get(sc->dev, "typec_cc2_cal");
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	memcpy(&calib_data, buf, min(len, sizeof(u32)));
	calib_data = (calib_data & SC27XX_CC2_MASK(sc->var_data->efuse_cc2_shift))
			>> SC27XX_CC_SHIFT(sc->var_data->efuse_cc2_shift);
	kfree(buf);

	return calib_data;
}

static int sprd_pmic_typec_set_rtrim(struct sprd_pmic_typec *sc)
{
	int calib_cc1;
	int calib_cc2;
	u32 rtrim;

	calib_cc1 = sprd_pmic_typec_get_cc1_efuse(sc);
	if (calib_cc1 < 0)
		return calib_cc1;
	calib_cc2 = sprd_pmic_typec_get_cc2_efuse(sc);
	if (calib_cc2 < 0)
		return calib_cc2;

	rtrim = calib_cc1 | calib_cc2<<5;

	return regmap_write(sc->regmap, sc->base + SC27XX_RTRIM, rtrim);
}

static ssize_t
sprd_pmic_typec_cc_polarity_role_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct sprd_pmic_typec *sc = dev_get_drvdata(dev);

	return snprintf(buf, 5, "%s\n", sprd_pmic_typec_cc_polarity_roles[sc->cc_polarity]);
}
static DEVICE_ATTR_RO(sprd_pmic_typec_cc_polarity_role);

static struct attribute *sprd_pmic_typec_attrs[] = {
	&dev_attr_sprd_pmic_typec_cc_polarity_role.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sprd_pmic_typec);

static int sprd_pmic_typec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct sprd_pmic_typec *sc;
	const struct sprd_pmic_typec_variant_data *pdata;
	int mode, ret;

	pdata = of_device_get_match_data(dev);
	if (!pdata) {
		dev_err(&pdev->dev, "No matching driver data found\n");
		return -EINVAL;
	}

	sc = devm_kzalloc(&pdev->dev, sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	sc->edev = devm_extcon_dev_allocate(&pdev->dev, sprd_pmic_typec_cable);
	if (IS_ERR(sc->edev)) {
		dev_err(&pdev->dev, "failed to allocate extcon device\n");
		return PTR_ERR(sc->edev);
	}

	ret = devm_extcon_dev_register(&pdev->dev, sc->edev);
	if (ret < 0) {
		dev_err(&pdev->dev, "can't register extcon device: %d\n", ret);
		return ret;
	}

	sc->dev = &pdev->dev;
	sc->irq = platform_get_irq(pdev, 0);
	if (sc->irq < 0) {
		dev_err(sc->dev, "failed to get typec interrupt.\n");
		return sc->irq;
	}

	sc->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!sc->regmap) {
		dev_err(sc->dev, "failed to get regmap.\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(node, "reg", &sc->base);
	if (ret) {
		dev_err(dev, "failed to get reg offset!\n");
		return ret;
	}

	ret = of_property_read_u32(node, "sprd,mode", &mode);
	if (ret) {
		dev_err(dev, "failed to get typec port mode type\n");
		return ret;
	}

	if (mode < TYPEC_PORT_DFP || mode > TYPEC_PORT_DRP
	    || mode == TYPEC_PORT_UFP) {
		mode = TYPEC_PORT_UFP;
		sc->usb20_only = true;
		dev_info(dev, "usb 2.0 only is enabled\n");
	}

	sc->var_data = pdata;
	sc->mode = mode;
	sc->typec_cap.type = mode;
	sc->typec_cap.data = TYPEC_PORT_DRD;
	sc->typec_cap.revision = USB_TYPEC_REV_1_2;
	sc->typec_cap.prefer_role = TYPEC_NO_PREFERRED_ROLE;
	sc->port = typec_register_port(&pdev->dev, &sc->typec_cap);
	if (!sc->port) {
		dev_err(sc->dev, "failed to register port!\n");
		return -ENODEV;
	}
	spin_lock_init(&sc->lock);

	ret = sysfs_create_groups(&sc->dev->kobj, sprd_pmic_typec_groups);
	if (ret < 0)
		dev_err(sc->dev, "failed to create cc_polarity %d\n", ret);

	ret = sprd_pmic_typec_set_rtrim(sc);
	if (ret < 0) {
		dev_err(sc->dev, "failed to set typec rtrim %d\n", ret);
		goto error;
	}

	ret = devm_request_threaded_irq(sc->dev, sc->irq, NULL,
					sprd_pmic_typec_interrupt,
					IRQF_EARLY_RESUME | IRQF_ONESHOT,
					dev_name(sc->dev), sc);
	if (ret) {
		dev_err(sc->dev, "failed to request irq %d\n", ret);
		goto error;
	}

	ret = sprd_pmic_typec_enable(sc);
	if (ret)
		goto error;

	platform_set_drvdata(pdev, sc);
	return 0;

error:
	typec_unregister_port(sc->port);
	return ret;
}

static int sprd_pmic_typec_remove(struct platform_device *pdev)
{
	struct sprd_pmic_typec *sc = platform_get_drvdata(pdev);

	sysfs_remove_groups(&sc->dev->kobj, sprd_pmic_typec_groups);
	sprd_pmic_typec_disconnect(sc, 0);
	typec_unregister_port(sc->port);

	return 0;
}

static const struct of_device_id sprd_pmic_typec_match[] = {
	{.compatible = "sprd,sc2730-typec", .data = &sc2730_data},
	{},
};
MODULE_DEVICE_TABLE(of, sprd_pmic_typec_match);

static struct platform_driver sprd_pmic_typec_driver = {
	.probe = sprd_pmic_typec_probe,
	.remove = sprd_pmic_typec_remove,
	.driver = {
		.name = "sprd-typec",
		.of_match_table = sprd_pmic_typec_match,
	},
};
module_platform_driver(sprd_pmic_typec_driver);
MODULE_DESCRIPTION("unisoc sprd_pmic_typec driver");
MODULE_AUTHOR("XinHu Wu <xinhu.wu@unisoc.com>");
MODULE_LICENSE("GPL");
