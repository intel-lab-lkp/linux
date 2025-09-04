// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Sartura Ltd.
 *
 * Driver for the TI TPS23861 PoE PSE.
 *
 * Author: Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>

#define MAX_SUPPORTED_POE_CLASS		4
#define INTERRUPT_STATUS		0x0
#define INTERRUPT_ENABLE		0x1
#define INTERRUPT_CLASS			BIT(4)
#define DETECTION_EVENT			0x5
#define POWER_STATUS			0x10
#define PORT_1_ICUT			0x2A
#define PORT_1_ICUT_MASK		GENMASK(2, 0)
#define RESET				0x1a
#define RESET_CLRAIN			0x80
#define TEMPERATURE			0x2c
#define INPUT_VOLTAGE_LSB		0x2e
#define INPUT_VOLTAGE_MSB		0x2f
#define PORT_1_CURRENT_LSB		0x30
#define PORT_1_CURRENT_MSB		0x31
#define PORT_1_VOLTAGE_LSB		0x32
#define PORT_1_VOLTAGE_MSB		0x33
#define PORT_2_CURRENT_LSB		0x34
#define PORT_2_CURRENT_MSB		0x35
#define PORT_2_VOLTAGE_LSB		0x36
#define PORT_2_VOLTAGE_MSB		0x37
#define PORT_3_CURRENT_LSB		0x38
#define PORT_3_CURRENT_MSB		0x39
#define PORT_3_VOLTAGE_LSB		0x3a
#define PORT_3_VOLTAGE_MSB		0x3b
#define PORT_4_CURRENT_LSB		0x3c
#define PORT_4_CURRENT_MSB		0x3d
#define PORT_4_VOLTAGE_LSB		0x3e
#define PORT_4_VOLTAGE_MSB		0x3f
#define PORT_N_CURRENT_LSB_OFFSET	0x04
#define PORT_N_VOLTAGE_LSB_OFFSET	0x04
#define VOLTAGE_CURRENT_MASK		GENMASK(13, 0)
#define PORT_1_RESISTANCE_LSB		0x60
#define PORT_1_RESISTANCE_MSB		0x61
#define PORT_2_RESISTANCE_LSB		0x62
#define PORT_2_RESISTANCE_MSB		0x63
#define PORT_3_RESISTANCE_LSB		0x64
#define PORT_3_RESISTANCE_MSB		0x65
#define PORT_4_RESISTANCE_LSB		0x66
#define PORT_4_RESISTANCE_MSB		0x67
#define PORT_N_RESISTANCE_LSB_OFFSET	0x02
#define PORT_RESISTANCE_MASK		GENMASK(13, 0)
#define PORT_RESISTANCE_RSN_MASK	GENMASK(15, 14)
#define PORT_RESISTANCE_RSN_OTHER	0
#define PORT_RESISTANCE_RSN_LOW		1
#define PORT_RESISTANCE_RSN_OPEN	2
#define PORT_RESISTANCE_RSN_SHORT	3
#define PORT_1_STATUS			0x0c
#define PORT_2_STATUS			0x0d
#define PORT_3_STATUS			0x0e
#define PORT_4_STATUS			0x0f
#define PORT_STATUS_CLASS_MASK		GENMASK(7, 4)
#define PORT_STATUS_DETECT_MASK		GENMASK(3, 0)
#define PORT_CLASS_UNKNOWN		0
#define PORT_CLASS_1			1
#define PORT_CLASS_2			2
#define PORT_CLASS_3			3
#define PORT_CLASS_4			4
#define PORT_CLASS_RESERVED		5
#define PORT_CLASS_0			6
#define PORT_CLASS_OVERCURRENT		7
#define PORT_CLASS_MISMATCH		8
#define PORT_DETECT_UNKNOWN		0
#define PORT_DETECT_SHORT		1
#define PORT_DETECT_RESERVED		2
#define PORT_DETECT_RESISTANCE_LOW	3
#define PORT_DETECT_RESISTANCE_OK	4
#define PORT_DETECT_RESISTANCE_HIGH	5
#define PORT_DETECT_OPEN_CIRCUIT	6
#define PORT_DETECT_RESERVED_2		7
#define PORT_DETECT_MOSFET_FAULT	8
#define PORT_DETECT_LEGACY		9
/* Measurment beyond clamp voltage */
#define PORT_DETECT_CAPACITANCE_INVALID_BEYOND	10
/* Insufficient voltage delta */
#define PORT_DETECT_CAPACITANCE_INVALID_DELTA	11
#define PORT_DETECT_CAPACITANCE_OUT_OF_RANGE	12
#define POE_PLUS			0x40
#define OPERATING_MODE			0x12
#define OPERATING_MODE_OFF		0
#define OPERATING_MODE_MANUAL		1
#define OPERATING_MODE_SEMI		2
#define OPERATING_MODE_AUTO		3
#define OPERATING_MODE_PORT_1_MASK	GENMASK(1, 0)
#define OPERATING_MODE_PORT_2_MASK	GENMASK(3, 2)
#define OPERATING_MODE_PORT_3_MASK	GENMASK(5, 4)
#define OPERATING_MODE_PORT_4_MASK	GENMASK(7, 6)

#define DETECT_CLASS_RESTART		0x18
#define POWER_ENABLE			0x19
#define TPS23861_NUM_PORTS		4

#define TPS23861_GENERAL_MASK_1		0x17
#define TPS23861_CURRENT_SHUNT_MASK	BIT(0)

#define TPS23861_TIME_RESET_US		5
#define TPS23861_TIME_RESET_US_MAX	1000

#define TPS23861_TIME_POWER_ON_RESET_MS 23
#define TPS23861_TIME_I2C_MS		20

#define TEMPERATURE_LSB			652 /* 0.652 degrees Celsius */
#define VOLTAGE_LSB			3662 /* 3.662 mV */
#define SHUNT_RESISTOR_DEFAULT		255000 /* 255 mOhm */
#define CURRENT_LSB_250			62260 /* 62.260 uA */
#define CURRENT_LSB_255			61039 /* 61.039 uA */
#define RESISTANCE_LSB			110966 /* 11.0966 Ohm*/
#define RESISTANCE_LSB_LOW		157216 /* 15.7216 Ohm*/

struct tps23861_port_data { // From DT.
	const char *label;
	/* Maximum class accepted by the port. The hardware cannot be
	 * preconfigured to accept certain class only, so classification
	 * interrupt handler is required to for power-on decision if class is
	 * not MAX_SUPPORTED_POE_CLASS.
	 */
	u32 class;
	/* true if the port is disabled in the DT */
	bool fully_disabled;
	/* true if the port shouldn't be enabled on driver init */
	bool off_by_default;
};

struct tps23861_data {
	struct regmap *regmap;
	u32 shunt_resistor;
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *shutdown_gpio;
	int irq;
	struct tps23861_port_data ports[TPS23861_NUM_PORTS];
};

static const struct regmap_config tps23861_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x6f,
};

static int tps23861_read_temp(struct tps23861_data *data, long *val)
{
	unsigned int regval;
	int err;

	err = regmap_read(data->regmap, TEMPERATURE, &regval);
	if (err < 0)
		return err;

	*val = ((long)regval * TEMPERATURE_LSB) - 20000;

	return 0;
}

static int tps23861_read_voltage(struct tps23861_data *data, int channel,
				 long *val)
{
	__le16 regval;
	long raw_val;
	int err;

	if (channel < TPS23861_NUM_PORTS) {
		err = regmap_bulk_read(data->regmap,
				       PORT_1_VOLTAGE_LSB + channel * PORT_N_VOLTAGE_LSB_OFFSET,
				       &regval, 2);
	} else {
		err = regmap_bulk_read(data->regmap,
				       INPUT_VOLTAGE_LSB,
				       &regval, 2);
	}
	if (err < 0)
		return err;

	raw_val = le16_to_cpu(regval);
	*val = (FIELD_GET(VOLTAGE_CURRENT_MASK, raw_val) * VOLTAGE_LSB) / 1000;

	return 0;
}

static int tps23861_read_current(struct tps23861_data *data, int channel,
				 long *val)
{
	long raw_val, current_lsb;
	__le16 regval;

	int err;

	if (data->shunt_resistor == SHUNT_RESISTOR_DEFAULT)
		current_lsb = CURRENT_LSB_255;
	else
		current_lsb = CURRENT_LSB_250;

	err = regmap_bulk_read(data->regmap,
			       PORT_1_CURRENT_LSB + channel * PORT_N_CURRENT_LSB_OFFSET,
			       &regval, 2);
	if (err < 0)
		return err;

	raw_val = le16_to_cpu(regval);
	*val = (FIELD_GET(VOLTAGE_CURRENT_MASK, raw_val) * current_lsb) / 1000000;

	return 0;
}

static int tps23861_port_disable(struct tps23861_data *data, int channel)
{
	unsigned int regval = 0;
	int err;

	regval |= BIT(channel + 4);
	err = regmap_write(data->regmap, POWER_ENABLE, regval);

	return err;
}

static int tps23861_port_enable(struct tps23861_data *data, int channel)
{
	unsigned int regval = 0;
	int err;

	regval |= BIT(channel);
	regval |= BIT(channel + 4);
	err = regmap_write(data->regmap, DETECT_CLASS_RESTART, regval);
	if (err)
		return err;
	return regmap_write(data->regmap, RESET, RESET_CLRAIN);
}

static umode_t tps23861_is_visible(const void *input_data, enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	const struct tps23861_data *data = input_data;

	/* Channel may be >=TPS23861_NUM_PORTS for common Input voltage */
	if (channel < TPS23861_NUM_PORTS && data->ports[channel].fully_disabled)
		return 0;
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			return 0444;
		default:
			return 0;
		}
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			return 0444;
		case hwmon_in_enable:
			return 0200;
		default:
			return 0;
		}
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
		case hwmon_curr_label:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int tps23861_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	struct tps23861_data *data = dev_get_drvdata(dev);
	int err;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_enable:
			if (val == 0)
				err = tps23861_port_disable(data, channel);
			else if (val == 1)
				err = tps23861_port_enable(data, channel);
			else
				err = -EINVAL;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static int tps23861_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct tps23861_data *data = dev_get_drvdata(dev);
	int err;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			err = tps23861_read_temp(data, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			err = tps23861_read_voltage(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			err = tps23861_read_current(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static const char * const tps23861_port_label[] = {
	"Port1",
	"Port2",
	"Port3",
	"Port4",
	"Input",
};

static int tps23861_read_string(struct device *dev,
				enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	struct tps23861_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_in:
	case hwmon_curr:
		if (channel < TPS23861_NUM_PORTS)
			*str = data->ports[channel].label;
		else
			*str = tps23861_port_label[channel];
		break;
	case hwmon_temp:
		*str = "Die";
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_channel_info * const tps23861_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_ops tps23861_hwmon_ops = {
	.is_visible = tps23861_is_visible,
	.write = tps23861_write,
	.read = tps23861_read,
	.read_string = tps23861_read_string,
};

static const struct hwmon_chip_info tps23861_chip_info = {
	.ops = &tps23861_hwmon_ops,
	.info = tps23861_info,
};

static char *port_operating_mode_string(uint8_t mode_reg, unsigned int port)
{
	unsigned int mode = ~0;

	if (port < TPS23861_NUM_PORTS)
		mode = (mode_reg >> (2 * port)) & OPERATING_MODE_PORT_1_MASK;

	switch (mode) {
	case OPERATING_MODE_OFF:
		return "Off";
	case OPERATING_MODE_MANUAL:
		return "Manual";
	case OPERATING_MODE_SEMI:
		return "Semi-Auto";
	case OPERATING_MODE_AUTO:
		return "Auto";
	default:
		return "Invalid";
	}
}

static char *port_detect_status_string(uint8_t status_reg)
{
	switch (FIELD_GET(PORT_STATUS_DETECT_MASK, status_reg)) {
	case PORT_DETECT_UNKNOWN:
		return "Unknown device";
	case PORT_DETECT_SHORT:
		return "Short circuit";
	case PORT_DETECT_RESISTANCE_LOW:
		return "Too low resistance";
	case PORT_DETECT_RESISTANCE_OK:
		return "Valid resistance";
	case PORT_DETECT_RESISTANCE_HIGH:
		return "Too high resistance";
	case PORT_DETECT_OPEN_CIRCUIT:
		return "Open circuit";
	case PORT_DETECT_MOSFET_FAULT:
		return "MOSFET fault";
	case PORT_DETECT_LEGACY:
		return "Legacy device";
	case PORT_DETECT_CAPACITANCE_INVALID_BEYOND:
		return "Invalid capacitance, beyond clamp voltage";
	case PORT_DETECT_CAPACITANCE_INVALID_DELTA:
		return "Invalid capacitance, insufficient voltage delta";
	case PORT_DETECT_CAPACITANCE_OUT_OF_RANGE:
		return "Valid capacitance, outside of legacy range";
	case PORT_DETECT_RESERVED:
	case PORT_DETECT_RESERVED_2:
	default:
		return "Invalid";
	}
}

static char *port_class_status_string(uint8_t status_reg)
{
	switch (FIELD_GET(PORT_STATUS_CLASS_MASK, status_reg)) {
	case PORT_CLASS_UNKNOWN:
		return "Unknown";
	case PORT_CLASS_RESERVED:
	case PORT_CLASS_0:
		return "0";
	case PORT_CLASS_1:
		return "1";
	case PORT_CLASS_2:
		return "2";
	case PORT_CLASS_3:
		return "3";
	case PORT_CLASS_4:
		return "4";
	case PORT_CLASS_OVERCURRENT:
		return "Overcurrent";
	case PORT_CLASS_MISMATCH:
		return "Mismatch";
	default:
		return "Invalid";
	}
}

static char *port_poe_plus_status_string(uint8_t poe_plus, unsigned int port)
{
	return (BIT(port + 4) & poe_plus) ? "Yes" : "No";
}

static int tps23861_port_resistance(struct tps23861_data *data, int port)
{
	unsigned int raw_val;
	__le16 regval;

	regmap_bulk_read(data->regmap,
			 PORT_1_RESISTANCE_LSB + PORT_N_RESISTANCE_LSB_OFFSET * port,
			 &regval,
			 2);

	raw_val = le16_to_cpu(regval);
	switch (FIELD_GET(PORT_RESISTANCE_RSN_MASK, raw_val)) {
	case PORT_RESISTANCE_RSN_OTHER:
		return (FIELD_GET(PORT_RESISTANCE_MASK, raw_val) * RESISTANCE_LSB) / 10000;
	case PORT_RESISTANCE_RSN_LOW:
		return (FIELD_GET(PORT_RESISTANCE_MASK, raw_val) * RESISTANCE_LSB_LOW) / 10000;
	case PORT_RESISTANCE_RSN_SHORT:
	case PORT_RESISTANCE_RSN_OPEN:
	default:
		return 0;
	}
}

static int tps23861_port_status_show(struct seq_file *s, void *data)
{
	struct tps23861_data *priv = s->private;
	unsigned int i, mode, poe_plus, status;

	regmap_read(priv->regmap, OPERATING_MODE, &mode);
	regmap_read(priv->regmap, POE_PLUS, &poe_plus);

	for (i = 0; i < TPS23861_NUM_PORTS; i++) {
		regmap_read(priv->regmap, PORT_1_STATUS + i, &status);

		seq_printf(s, "Port: \t\t%d\n", i + 1);
		seq_printf(s, "Operating mode: %s\n", port_operating_mode_string(mode, i));
		seq_printf(s, "Detected: \t%s\n", port_detect_status_string(status));
		seq_printf(s, "Class: \t\t%s\n", port_class_status_string(status));
		seq_printf(s, "PoE Plus: \t%s\n", port_poe_plus_status_string(poe_plus, i));
		seq_printf(s, "Resistance: \t%d\n", tps23861_port_resistance(priv, i));
		seq_putc(s, '\n');
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tps23861_port_status);

static inline bool tps23861_auto_mode(struct tps23861_port_data *port)
{
	return port->class == MAX_SUPPORTED_POE_CLASS;
}

static irqreturn_t tps23861_irq_handler(int irq, void *dev_id)
{
	struct tps23861_data *data = (struct tps23861_data *)dev_id;
	struct tps23861_port_data *ports = data->ports;
	struct device *dev = &data->client->dev;

	unsigned int intr_status, status, class, i;
	unsigned int det_status = 0;
	int ret;

	ret = regmap_read(data->regmap, INTERRUPT_STATUS, &intr_status);
	if (ret || intr_status == 0)
		return IRQ_NONE;
	if (intr_status & INTERRUPT_CLASS) {
		regmap_read(data->regmap, DETECTION_EVENT, &det_status);
		for (i = 0; i < TPS23861_NUM_PORTS; i++) {
			if (tps23861_auto_mode(ports + i))
				continue;
			if (!(det_status & BIT(i + 4)))
				continue;
			ret = regmap_read(data->regmap, PORT_1_STATUS + i, &status);
			if (ret)
				continue;
			class = FIELD_GET(PORT_STATUS_CLASS_MASK, status);
			if (class == PORT_CLASS_0) {
				/* class 0 is same power as class 3, change 0 to
				 * 3 for easy comparison
				 */
				class = 3;
			}
			if (class == PORT_CLASS_UNKNOWN ||
			    class > MAX_SUPPORTED_POE_CLASS)
				continue;
			if (class > ports[i].class) {
				dev_info(dev, "%s class mismatch: got %d, want <= %d",
					 ports[i].label, class, ports[i].class);
				continue;
			}
			regmap_read(data->regmap, POWER_STATUS, &status);
			if (status & BIT(i))
				continue; /* already enabled */
			/* Set ICUT and poe+ according to class. Values in ICUT table happen
			 * to match class values, so just set class.
			 */
			regmap_update_bits(data->regmap,
					   PORT_1_ICUT + i / 2,
					   PORT_1_ICUT_MASK << ((i % 2) * 4),
					   class << ((i % 2) * 4));
			regmap_update_bits(data->regmap, POE_PLUS,
					   BIT(i + 4),
					   class > 3 ? BIT(i + 4) : 0);
			dev_info(dev, "%s class %d, enabling power",
				 ports[i].label, class);
			regmap_write(data->regmap, POWER_ENABLE, BIT(i));
		}
	}
	regmap_write(data->regmap, RESET, RESET_CLRAIN);
	return IRQ_HANDLED;
}

static int tps23861_reset(struct i2c_client *client)
{
	struct tps23861_data *data = i2c_get_clientdata(client);
	unsigned int val;

	if (data->reset_gpio) {
		gpiod_direction_output(data->reset_gpio, true);
		/* If shutdown pin is defined, use it to keep ports off, while
		 * turning the chip on for i2c configuration.
		 */
		if (data->shutdown_gpio)
			gpiod_direction_output(data->shutdown_gpio, true);
		usleep_range(TPS23861_TIME_RESET_US, TPS23861_TIME_RESET_US_MAX);
		gpiod_set_value_cansleep(data->reset_gpio, false);
		msleep(TPS23861_TIME_POWER_ON_RESET_MS);
		if (data->shutdown_gpio)
			gpiod_set_value_cansleep(data->shutdown_gpio, false);
		msleep(TPS23861_TIME_I2C_MS);
	}

	/* Check the device is responsive */
	return regmap_read(data->regmap, OPERATING_MODE, &val);
}

static void tps23861_init_ports(struct i2c_client *client)
{
	struct tps23861_data *data = i2c_get_clientdata(client);
	struct tps23861_port_data *ports = data->ports;
	unsigned int i, mode;

	for (i = 0; i < TPS23861_NUM_PORTS; i++) {
		mode = ports[i].fully_disabled	     ? OPERATING_MODE_OFF :
		       tps23861_auto_mode(&ports[i]) ? OPERATING_MODE_AUTO :
						       OPERATING_MODE_SEMI;
		regmap_update_bits(data->regmap, OPERATING_MODE,
				   OPERATING_MODE_PORT_1_MASK << (2 * i),
				   mode << (2 * i));
		if (ports[i].fully_disabled)
			continue;
		if (ports[i].off_by_default)
			tps23861_port_disable(data, i);
		else
			tps23861_port_enable(data, i);
	}
}

static int tps23861_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tps23861_data *data;
	struct tps23861_port_data *ports;
	struct device *hwmon_dev;
	struct gpio_desc *gpio;
	struct device_node *child;
	u32 shunt_resistor;
	int ret;
	unsigned int i;
	bool need_irq = false;
	const char *hwmon_name = client->name;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ports = data->ports;
	for (i = 0; i < TPS23861_NUM_PORTS; i++) {
		ports[i].label = tps23861_port_label[i];
		ports[i].class = MAX_SUPPORTED_POE_CLASS;
	}

	gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(gpio)) {
		devm_kfree(dev, data);
		return -EPROBE_DEFER;
	}
	data->reset_gpio = gpio;

	gpio = devm_gpiod_get_optional(dev, "ti,ports-shutdown", GPIOD_ASIS);
	if (IS_ERR(gpio)) {
		devm_kfree(dev, data);
		return -EPROBE_DEFER;
	}
	data->shutdown_gpio = gpio;

	data->client = client;
	i2c_set_clientdata(client, data);

	data->regmap = devm_regmap_init_i2c(client, &tps23861_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(dev, "failed to allocate register map\n");
		return PTR_ERR(data->regmap);
	}
	ret = tps23861_reset(client);
	if (ret)
		return -ENODEV;
	for_each_child_of_node(dev->of_node, child) {
		ret = of_property_read_u32(child, "reg", &i);
		if (ret || i >= TPS23861_NUM_PORTS) {
			dev_err(dev, "node %s must define 'reg' < %d\n",
				child->name, TPS23861_NUM_PORTS);
			continue;
		}
		if (!of_device_is_available(child)) {
			ports[i].fully_disabled = true;
			continue;
		}
		ports[i].off_by_default = of_property_read_bool(child, "ti,off-by-default");
		of_property_read_string(child, "label", &ports[i].label);
		of_property_read_u32(child, "ti,class", &ports[i].class);
		if (ports[i].class > MAX_SUPPORTED_POE_CLASS) {
			dev_err(dev, "%s invalid class, disabling\n", ports[i].label);
			ports[i].fully_disabled = true;
			continue;
		}
		if (ports[i].class == 0) {
			/* class 0 is same power as class 3, change 0 to 3 for
			 * easy comparison
			 */
			ports[i].class = 3;
		}
		need_irq = need_irq || !tps23861_auto_mode(&ports[i]);
		dev_info(dev, "%s: max class: %d, %s by default\n",
			 ports[i].label, ports[i].class,
			 ports[i].off_by_default ? "off" : "on");
	}
	if (need_irq) {
		data->irq = irq_of_parse_and_map(dev->of_node, 0);
		if (!data->irq) {
			dev_err(dev, "failed to configure irq\n");
			return -EINVAL;
		}
		ret = devm_request_threaded_irq(dev, data->irq, NULL,
						tps23861_irq_handler,
						IRQF_TRIGGER_FALLING,
						"tps23861_irq", data);
		if (ret) {
			dev_err(dev, "failed to request irq %d\n", data->irq);
			return ret;
		}
		regmap_write(data->regmap, INTERRUPT_ENABLE, INTERRUPT_CLASS);
	}

	if (!of_property_read_u32(dev->of_node, "shunt-resistor-micro-ohms", &shunt_resistor))
		data->shunt_resistor = shunt_resistor;
	else
		data->shunt_resistor = SHUNT_RESISTOR_DEFAULT;

	if (data->shunt_resistor == SHUNT_RESISTOR_DEFAULT)
		regmap_clear_bits(data->regmap,
				  TPS23861_GENERAL_MASK_1,
				  TPS23861_CURRENT_SHUNT_MASK);
	else
		regmap_set_bits(data->regmap,
				TPS23861_GENERAL_MASK_1,
				TPS23861_CURRENT_SHUNT_MASK);

	of_property_read_string(dev->of_node, "label", &hwmon_name);

	tps23861_init_ports(client);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, hwmon_name,
							 data, &tps23861_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	debugfs_create_file("port_status", 0400, client->debugfs, data,
			    &tps23861_port_status_fops);

	return 0;
}

static const struct of_device_id __maybe_unused tps23861_of_match[] = {
	{ .compatible = "ti,tps23861", },
	{ },
};
MODULE_DEVICE_TABLE(of, tps23861_of_match);

static struct i2c_driver tps23861_driver = {
	.probe			= tps23861_probe,
	.driver = {
		.name		= "tps23861",
		.of_match_table	= of_match_ptr(tps23861_of_match),
	},
};
module_i2c_driver(tps23861_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("TI TPS23861 PoE PSE");
