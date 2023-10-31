// SPDX-License-Identifier: GPL-2.0+
/*
 * HWMON driver for MEC172x chip that publishes some sensor values
 * via the embedded controller registers specific to Lenovo Systems.
 *
 * Copyright (C) 2023 David Ober (Lenovo) <dober@lenovo.com>
 *
 * EC provides:
 * - CPU temperature
 * - DIMM temperature
 * - Chassis zone temperatures
 * - CPU fan RPM
 * - DIMM fan RPM
 * - Chassis fans RPM
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/units.h>

#define MCHP_SING_IDX			0x0000
#define MCHP_EMI0_APPLICATION_ID	0x090C
#define MCHP_EMI0_EC_ADDRESS_LSB	0x0902
#define MCHP_EMI0_EC_ADDRESS_MSB	0x0903
#define MCHP_EMI0_EC_DATA_BYTE0		0x0904
#define MCHP_EMI0_EC_DATA_BYTE1		0x0905
#define MCHP_EMI0_EC_DATA_BYTE2		0x0906
#define MCHP_EMI0_EC_DATA_BYTE3		0x0907

#define io_write8(a, b)	outb_p(b, a)
#define io_read8(a)	inb_p(a)

static inline uint8_t
get_ec_reg(unsigned char page, unsigned char index)
{
	u8 onebyte;
	unsigned short m_index;
	unsigned short phy_index = page * 256 + index;

	if (io_read8(MCHP_EMI0_APPLICATION_ID) != 0) /* EMI access locked */
		return -1;

	io_write8(MCHP_EMI0_APPLICATION_ID, 0x01);

	m_index = phy_index & 0x7FFC;
	io_write8(MCHP_EMI0_EC_ADDRESS_LSB, m_index);
	io_write8(MCHP_EMI0_EC_ADDRESS_MSB, m_index >> 8);

	switch (phy_index & 0x0003) {
	case 0:
		onebyte = io_read8(MCHP_EMI0_EC_DATA_BYTE0);
		break;
	case 1:
		onebyte = io_read8(MCHP_EMI0_EC_DATA_BYTE1);
		break;
	case 2:
		onebyte = io_read8(MCHP_EMI0_EC_DATA_BYTE2);
		break;
	case 3:
		onebyte = io_read8(MCHP_EMI0_EC_DATA_BYTE3);
		break;
	}

	io_write8(MCHP_EMI0_APPLICATION_ID, 0x01);  /* write same data to clean */
	return onebyte;
}

static const char * const systems[] = {
	"Tomcat",
	"Hornet",
	"Falcon",
	"Manta_",
};

static const char * const lenovo_px_ec_temp_label[] = {
	"CPU1",
	"CPU2",
	"R_DIMM1",
	"L_DIMM1",
	"R_DIMM2",
	"L_DIMM2",
	"PCH",
	"M2_R",
	"M2_Z1R",
	"M2_Z2R",
	"PCI_Z1",
	"PCI_Z2",
	"PCI_Z3",
	"PCI_Z4",
	"AMB",
};

static const char * const lenovo_gen_ec_temp_label[] = {
	"CPU1",
	"",
	"R_DIMM",
	"L_DIMM",
	"",
	"",
	"PCH",
	"M2_R",
	"M2_Z1R",
	"M2_Z2R",
	"PCI_Z1",
	"PCI_Z2",
	"PCI_Z3",
	"PCI_Z4",
	"AMB",
};

static const char * const px_ec_fan_label[] = {
	"CPU1_Fan",
	"CPU2_Fan",
	"Front_Fan1-1",
	"Front_Fan1-2",
	"Front_Fan2",
	"Front_Fan3",
	"MEM_Fan1",
	"MEM_Fan2",
	"Rear_Fan1",
	"Rear_Fan2",
	"Flex_Bay_Fan1",
	"Flex_Bay_Fan2",
	"Flex_Bay_Fan2",
	"PSU_HDD_Fan",
	"PSU1_Fan",
	"PSU2_Fan",
};

static const char * const p7_ec_fan_label[] = {
	"CPU1_Fan",
	"",
	"HP_CPU_Fan1",
	"HP_CPU_Fan2",
	"PCIE1_4_Fan",
	"PCIE5_7_Fan",
	"MEM_Fan1",
	"MEM_Fan2",
	"Rear_Fan1",
	"",
	"BCB_Fan",
	"Flex_Bay_Fan",
	"",
	"",
	"PSU_Fan",
	"",
};

static const char * const p5_ec_fan_label[] = {
	"CPU_Fan",
	"",
	"",
	"",
	"",
	"HDD_Fan",
	"Duct_Fan1",
	"MEM_Fan",
	"Rear_Fan",
	"",
	"Front_Fan",
	"Flex_Bay_Fan",
	"",
	"",
	"PSU_Fan",
	"",
};

static const char * const p7_amd_ec_fan_label[] = {
	"CPU1_Fan",
	"CPU2_Fan",
	"HP_CPU_Fan1",
	"HP_CPU_Fan2",
	"PCIE1_4_Fan",
	"PCIE5_7_Fan",
	"DIMM1_Fan1",
	"DIMM1_Fan2",
	"DIMM2_Fan1",
	"DIMM2_Fan2",
	"Rear_Fan",
	"HDD_Bay_Fan",
	"Flex_Bay_Fan",
	"",
	"PSU_Fan",
	"",
};

struct ec_sensors_data {
	struct mutex mec_mutex; /* lock for sensors write */
	u8 platform_id;
	const char *const *fan_labels;
	const char *const *temp_labels;
};

static int
lenovo_ec_do_read_temp(struct ec_sensors_data *data, u32 attr, int channel, long *val)
{
	u8   LSB;

	switch (attr) {
	case hwmon_temp_input:
		mutex_lock(&data->mec_mutex);
		LSB = get_ec_reg(2, 0x81 + channel);
		mutex_unlock(&data->mec_mutex);
		if (LSB > 0x40) {
			*val = (LSB - 0x40) * 1000;
		} else {
			*val = 0;
			return -1;
		}
		return 0;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static int
lenovo_ec_do_read_fan(struct ec_sensors_data *data, u32 attr, int channel, long *val)
{
	u8    LSB, MSB;

	channel *= 2;
	switch (attr) {
	case hwmon_fan_input:
		mutex_lock(&data->mec_mutex);
		LSB = get_ec_reg(4, 0x60 + channel);
		MSB = get_ec_reg(4, 0x61 + channel);
		mutex_unlock(&data->mec_mutex);
		if ((MSB << 8) + LSB != 0) {
			mutex_lock(&data->mec_mutex);
			LSB = get_ec_reg(4, 0x20 + channel);
			MSB = get_ec_reg(4, 0x21 + channel);
			mutex_unlock(&data->mec_mutex);
			*val = (MSB << 8) + LSB;
			return 0;
		}
		return -1; /* Returning -1 here has the sensors tool mark the FAN speed as N/A */
	case hwmon_fan_max:
		mutex_lock(&data->mec_mutex);
		LSB = get_ec_reg(4, 0x60 + channel);
		MSB = get_ec_reg(4, 0x61 + channel);
		mutex_unlock(&data->mec_mutex);
		if ((MSB << 8) + LSB != 0) {
			mutex_lock(&data->mec_mutex);
			LSB = get_ec_reg(4, 0x40 + channel);
			MSB = get_ec_reg(4, 0x41 + channel);
			mutex_unlock(&data->mec_mutex);
			*val = (MSB << 8) + LSB;
		} else {
			*val = 0;
		}
		return 0;
	case hwmon_fan_min:
	case hwmon_fan_div:
	case hwmon_fan_alarm:
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static int get_platform(struct ec_sensors_data *data)
{
	char system_type[6];
	int ret = -1;
	int idx;

	for (idx = 0 ; idx < 6 ; idx++)
		mutex_lock(&data->mec_mutex);
		system_type[idx] = get_ec_reg(0xC, (0x10 + idx));
		mutex_unlock(&data->mec_mutex);

	for (idx = 0 ; idx < 4 ; idx++) {
		if (strcmp(systems[idx], system_type) == 0) {
			ret = idx;
			break;
		}
	}
	return ret;
}

static int
lenovo_ec_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, const char **str)
{
	struct ec_sensors_data *state = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		*str = state->temp_labels[channel];
		break;

	case hwmon_fan:
		*str = state->fan_labels[channel];
		break;
	default:
		return -EOPNOTSUPP; /* unreachable */
	}
	return 0;
}

static int
lenovo_ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *val)
{
	struct ec_sensors_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		return lenovo_ec_do_read_temp(data, attr, channel, val);
	case hwmon_fan:
		return lenovo_ec_do_read_fan(data, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t
lenovo_ec_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
			   u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input || attr == hwmon_fan_max || attr == hwmon_fan_label)
			return 0444;
		break;
	default:
		return 0;
	}
	return 0;
}

static const struct hwmon_channel_info *lenovo_ec_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_MAX),
	NULL
};

static const struct hwmon_ops lenovo_ec_hwmon_ops = {
	.is_visible = lenovo_ec_hwmon_is_visible,
	.read = lenovo_ec_hwmon_read,
	.read_string = lenovo_ec_hwmon_read_string,
};

static struct hwmon_chip_info lenovo_ec_chip_info = {
	.ops = &lenovo_ec_hwmon_ops,
	.info = lenovo_ec_hwmon_info,
};

static int lenovo_ec_probe(struct platform_device *pdev)
{
	struct device *hwdev;
	struct ec_sensors_data *ec_data;
	const struct hwmon_chip_info *chip_info;
	struct device *dev = &pdev->dev;

	ec_data = devm_kzalloc(dev, sizeof(struct ec_sensors_data), GFP_KERNEL);
	if (!ec_data)
		return -ENOMEM;

	dev_set_drvdata(dev, ec_data);

	chip_info = &lenovo_ec_chip_info;

	mutex_lock(&ec_data->mec_mutex);
	if (io_read8(MCHP_EMI0_APPLICATION_ID) != 0) { /* check EMI Application BIT */
		io_write8(0x90C, io_read8(0x90C)); /* set EMI Application BIT to 0 */
	}
	io_write8(MCHP_EMI0_EC_ADDRESS_LSB, MCHP_SING_IDX);
	io_write8(MCHP_EMI0_EC_ADDRESS_MSB, MCHP_SING_IDX >> 8);
	mutex_unlock(&ec_data->mec_mutex);

	if ((io_read8(MCHP_EMI0_EC_DATA_BYTE0) == 'M') &&
	    (io_read8(MCHP_EMI0_EC_DATA_BYTE1) == 'C') &&
	    (io_read8(MCHP_EMI0_EC_DATA_BYTE2) == 'H') &&
	    (io_read8(MCHP_EMI0_EC_DATA_BYTE3) == 'P')) {
		ec_data->platform_id = get_platform(ec_data);
		switch (ec_data->platform_id) {
		case 0:
			ec_data->fan_labels = px_ec_fan_label;
			ec_data->temp_labels = lenovo_px_ec_temp_label;
			break;
		case 1:
			ec_data->fan_labels = p7_ec_fan_label;
			ec_data->temp_labels = lenovo_gen_ec_temp_label;
			break;
		case 2:
			ec_data->fan_labels = p5_ec_fan_label;
			ec_data->temp_labels = lenovo_gen_ec_temp_label;
			break;
		case 3:
			ec_data->fan_labels = p7_amd_ec_fan_label;
			ec_data->temp_labels = lenovo_gen_ec_temp_label;
			break;
		default:
			dev_err(dev, "Unknown ThinkStation Model");
			return -EINVAL;
		}

		hwdev = devm_hwmon_device_register_with_info(dev, "lenovo_ec",
							     ec_data,
							     chip_info, NULL);

		return PTR_ERR_OR_ZERO(hwdev);
	}
	return -ENODEV;
}

static struct platform_driver lenovo_ec_sensors_platform_driver = {
	.driver = {
		.name	= "lenovo-ec-sensors",
	},
	.probe = lenovo_ec_probe,
};

static struct platform_device *lenovo_ec_sensors_platform_device;

static int __init lenovo_ec_init(void)
{
	lenovo_ec_sensors_platform_device =
		platform_create_bundle(&lenovo_ec_sensors_platform_driver,
				       lenovo_ec_probe, NULL, 0, NULL, 0);

	if (IS_ERR(lenovo_ec_sensors_platform_device))
		return PTR_ERR(lenovo_ec_sensors_platform_device);

	return 0;
}

static void __exit lenovo_ec_exit(void)
{
	platform_device_unregister(lenovo_ec_sensors_platform_device);
	platform_driver_unregister(&lenovo_ec_sensors_platform_driver);
}

module_init(lenovo_ec_init);
module_exit(lenovo_ec_exit);

MODULE_AUTHOR("David Ober <dober@lenovo.com>");
MODULE_DESCRIPTION("HWMON driver for MEC172x EC sensors on LENOVO motherboards");
MODULE_LICENSE("GPL");
