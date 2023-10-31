// SPDX-License-Identifier: GPL-2.0-only
/*
 * UP Board control CPLD/FPGA to provide more GPIO driving power
 * also provide CPLD LEDs and pin mux function
 * recognize HID AANT0F00 ~ AAANT0F04 in ACPI name space
 *
 * Copyright (c) AAEON. All rights reserved.
 *
 * Author: Gary Wang <garywang@aaeon.com.tw>
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/mfd/core.h>
#include <linux/mfd/upboard-fpga.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define RESET_DEVICE 1
#define ENABLE_DEVICE 1

#define AAEON_MANUFACTURER_ID		0x01
#define SUPPORTED_FW_MAJOR		0x0
#define MENUFACTURER_ID_MASK		GENMASK(7, 0)

#define FIRMWARE_ID_BUILD_OFFSET	12
#define FIRMWARE_ID_MAJOR_OFFSET	8
#define FIRMWARE_ID_MINOR_OFFSET	4
#define FIRMWARE_ID_PATCH_OFFSET	0
#define FIRMWARE_ID_MASK		GENMASK(3, 0)

/*
 * read CPLD register on custom protocol
 * send clock and addr bit in strobe and datain pins then read from dataout pin
 */
static int upboard_cpld_read(void *context, unsigned int reg, unsigned int *val)
{
	struct upboard_fpga * const fpga = context;
	int i;

	/* clear to start new transcation */
	gpiod_set_value(fpga->clear_gpio, 0);
	gpiod_set_value(fpga->clear_gpio, 1);

	reg |= UPFPGA_READ_FLAG;

	/* send clock and data from strobe & datain */
	for (i = UPFPGA_ADDRESS_SIZE; i >= 0; i--) {
		gpiod_set_value(fpga->strobe_gpio, 0);
		gpiod_set_value(fpga->datain_gpio, !!(reg & BIT(i)));
		gpiod_set_value(fpga->strobe_gpio, 1);
	}

	gpiod_set_value(fpga->strobe_gpio, 0);
	*val = 0;

	/* read from dataout */
	for (i = UPFPGA_REGISTER_SIZE - 1; i >= 0; i--) {
		gpiod_set_value(fpga->strobe_gpio, 1);
		gpiod_set_value(fpga->strobe_gpio, 0);
		*val |= gpiod_get_value(fpga->dataout_gpio) << i;
	}

	gpiod_set_value(fpga->strobe_gpio, 1);

	return 0;
}

/*
 * write CPLD register on custom protocol
 * send clock and addr bit in strobe and datain pins then write to datain pin
 */
static int upboard_cpld_write(void *context, unsigned int reg, unsigned int val)
{
	struct upboard_fpga * const fpga = context;
	int i;

	/* clear to start new transcation */
	gpiod_set_value(fpga->clear_gpio, 0);
	gpiod_set_value(fpga->clear_gpio, 1);

	/* send clock and data from strobe & datain */
	for (i = UPFPGA_ADDRESS_SIZE; i >= 0; i--) {
		gpiod_set_value(fpga->strobe_gpio, 0);
		gpiod_set_value(fpga->datain_gpio, !!(reg & BIT(i)));
		gpiod_set_value(fpga->strobe_gpio, 1);
	}

	gpiod_set_value(fpga->strobe_gpio, 0);

	/* write to datain pin */
	for (i = UPFPGA_REGISTER_SIZE - 1; i >= 0; i--) {
		gpiod_set_value(fpga->datain_gpio, !!(val & BIT(i)));
		gpiod_set_value(fpga->strobe_gpio, 1);
		gpiod_set_value(fpga->strobe_gpio, 0);
	}

	gpiod_set_value(fpga->strobe_gpio, 1);

	return 0;
}

static const struct regmap_range upboard_up_readable_ranges[] = {
	regmap_reg_range(UPFPGA_REG_PLATFORM_ID, UPFPGA_REG_FIRMWARE_ID),
	regmap_reg_range(UPFPGA_REG_FUNC_EN0, UPFPGA_REG_FUNC_EN0),
	regmap_reg_range(UPFPGA_REG_GPIO_DIR0, UPFPGA_REG_GPIO_DIR1),
};

static const struct regmap_range upboard_up_writable_ranges[] = {
	regmap_reg_range(UPFPGA_REG_FUNC_EN0, UPFPGA_REG_FUNC_EN0),
	regmap_reg_range(UPFPGA_REG_GPIO_DIR0, UPFPGA_REG_GPIO_DIR1),
};

static const struct regmap_access_table upboard_up_readable_table = {
	.yes_ranges = upboard_up_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(upboard_up_readable_ranges),
};

static const struct regmap_access_table upboard_up_writable_table = {
	.yes_ranges = upboard_up_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(upboard_up_writable_ranges),
};

static const struct regmap_config upboard_up_regmap_config = {
	.reg_bits = UPFPGA_ADDRESS_SIZE,
	.val_bits = UPFPGA_REGISTER_SIZE,
	.max_register = UPFPGA_REG_MAX,
	.reg_read = upboard_cpld_read,
	.reg_write = upboard_cpld_write,
	.fast_io = false,
	.cache_type = REGCACHE_NONE,
	.rd_table = &upboard_up_readable_table,
	.wr_table = &upboard_up_writable_table,
};

static struct upboard_led_data upboard_gpio_led_data[] = {
	{ .bit = 0, .colour = "gpio" },
};

/* 3 LEDs controlled by CPLD */
static struct upboard_led_data upboard_up_led_data[] = {
	{ .bit = 0, .colour = "yellow" },
	{ .bit = 1, .colour = "green" },
	{ .bit = 2, .colour = "red" },
};

static const struct mfd_cell upboard_up_mfd_cells[] = {
	MFD_CELL_NAME("upboard-pinctrl"),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up_led_data[0],
		       sizeof(*upboard_up_led_data), 0),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up_led_data[1],
		       sizeof(*upboard_up_led_data), 1),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up_led_data[2],
		       sizeof(*upboard_up_led_data), 2),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_gpio_led_data[0],
		       sizeof(*upboard_gpio_led_data), 0),
};

/* UP Squared 6000 EHL board */
static const struct mfd_cell upboard_pinctrl_cells[] = {
	MFD_CELL_BASIC("upboard-pinctrl", NULL, &upboard_gpio_led_data[0],
		       sizeof(*upboard_up_led_data), 0),
};

/* UP^2 board */
static const struct regmap_range upboard_up2_readable_ranges[] = {
	regmap_reg_range(UPFPGA_REG_PLATFORM_ID, UPFPGA_REG_FIRMWARE_ID),
	regmap_reg_range(UPFPGA_REG_FUNC_EN0, UPFPGA_REG_FUNC_EN1),
	regmap_reg_range(UPFPGA_REG_GPIO_EN0, UPFPGA_REG_GPIO_EN2),
	regmap_reg_range(UPFPGA_REG_GPIO_DIR0, UPFPGA_REG_GPIO_DIR2),
};

static const struct regmap_range upboard_up2_writable_ranges[] = {
	regmap_reg_range(UPFPGA_REG_FUNC_EN0, UPFPGA_REG_FUNC_EN1),
	regmap_reg_range(UPFPGA_REG_GPIO_EN0, UPFPGA_REG_GPIO_EN2),
	regmap_reg_range(UPFPGA_REG_GPIO_DIR0, UPFPGA_REG_GPIO_DIR2),
};

static const struct regmap_access_table upboard_up2_readable_table = {
	.yes_ranges = upboard_up2_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(upboard_up2_readable_ranges),
};

static const struct regmap_access_table upboard_up2_writable_table = {
	.yes_ranges = upboard_up2_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(upboard_up2_writable_ranges),
};

static const struct regmap_config upboard_up2_regmap_config = {
	.reg_bits = UPFPGA_ADDRESS_SIZE,
	.val_bits = UPFPGA_REGISTER_SIZE,
	.max_register = UPFPGA_REG_MAX,
	.reg_read = upboard_cpld_read,
	.reg_write = upboard_cpld_write,
	.fast_io = false,
	.cache_type = REGCACHE_NONE,
	.rd_table = &upboard_up2_readable_table,
	.wr_table = &upboard_up2_writable_table,
};

static struct upboard_led_data upboard_up2_led_data[] = {
	{ .bit = 0, .colour = "blue" },
	{ .bit = 1, .colour = "yellow" },
	{ .bit = 2, .colour = "green" },
	{ .bit = 3, .colour = "red" },
};

static const struct mfd_cell upboard_up2_mfd_cells[] = {
	MFD_CELL_NAME("upboard-pinctrl"),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[0],
		       sizeof(*upboard_up2_led_data), 0),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[1],
		       sizeof(*upboard_up2_led_data), 1),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[2],
		       sizeof(*upboard_up2_led_data), 2),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[3],
		       sizeof(*upboard_up2_led_data), 3),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_gpio_led_data[0],
		       sizeof(*upboard_gpio_led_data), 0),
};

static int __init upboard_cpld_gpio_init(struct upboard_fpga *fpga)
{
	enum gpiod_flags flags = fpga->uninitialised ? GPIOD_OUT_LOW : GPIOD_ASIS;

	fpga->enable_gpio = devm_gpiod_get(fpga->dev, "enable", flags);
	if (IS_ERR(fpga->enable_gpio))
		return PTR_ERR(fpga->enable_gpio);

	fpga->clear_gpio = devm_gpiod_get(fpga->dev, "clear", GPIOD_OUT_LOW);
	if (IS_ERR(fpga->clear_gpio))
		return PTR_ERR(fpga->clear_gpio);

	fpga->strobe_gpio = devm_gpiod_get(fpga->dev, "strobe", GPIOD_OUT_LOW);
	if (IS_ERR(fpga->strobe_gpio))
		return PTR_ERR(fpga->strobe_gpio);

	fpga->datain_gpio = devm_gpiod_get(fpga->dev, "datain", GPIOD_OUT_LOW);
	if (IS_ERR(fpga->datain_gpio))
		return PTR_ERR(fpga->datain_gpio);

	fpga->dataout_gpio = devm_gpiod_get(fpga->dev, "dataout", GPIOD_IN);
	if (IS_ERR(fpga->dataout_gpio))
		return PTR_ERR(fpga->dataout_gpio);

	/*
	 * The SoC pinctrl driver may not support reserving the GPIO line for
	 * FPGA reset without causing an undesired reset pulse. This will clear
	 * any settings on the FPGA, so only do it if we must.
	 * Reset GPIO defaults HIGH, get GPIO and set to LOW, then set back to
	 * HIGH as a pulse.
	 */
	if (fpga->uninitialised) {
		fpga->reset_gpio = devm_gpiod_get(fpga->dev, "reset", GPIOD_OUT_LOW);
		if (IS_ERR(fpga->reset_gpio))
			return PTR_ERR(fpga->reset_gpio);

		gpiod_set_value(fpga->reset_gpio, RESET_DEVICE);
	}

	gpiod_set_value(fpga->enable_gpio, ENABLE_DEVICE);
	fpga->uninitialised = false;

	return 0;
}

/* This function is for debugging with user for showing firmware information. */
static int __init upboard_fpga_verify_device(struct upboard_fpga *fpga)
{
	unsigned int platform_id, manufacturer_id;
	unsigned int firmware_id, build, major, minor, patch;
	int ret;

	ret = regmap_read(fpga->regmap, UPFPGA_REG_PLATFORM_ID, &platform_id);
	if (ret)
		return ret;

	manufacturer_id = platform_id & MENUFACTURER_ID_MASK;
	if (manufacturer_id != AAEON_MANUFACTURER_ID) {
		dev_err(fpga->dev,
			"Manufacturer ID 0x%02x not supported\n",
			manufacturer_id);

		return -ENODEV;
	}

	ret = regmap_read(fpga->regmap, UPFPGA_REG_FIRMWARE_ID, &firmware_id);
	if (ret)
		return ret;

	build = (firmware_id >> FIRMWARE_ID_BUILD_OFFSET) & FIRMWARE_ID_MASK;
	major = (firmware_id >> FIRMWARE_ID_MAJOR_OFFSET) & FIRMWARE_ID_MASK;
	minor = (firmware_id >> FIRMWARE_ID_MINOR_OFFSET) & FIRMWARE_ID_MASK;
	patch = (firmware_id >> FIRMWARE_ID_PATCH_OFFSET) & FIRMWARE_ID_MASK;

	if (major != SUPPORTED_FW_MAJOR) {
		dev_err(fpga->dev, "Manufacturer ID 0x%02x not supported\n", build);

		return -ENODEV;
	}

	dev_info(fpga->dev, "Compatible FPGA FW v%u.%u.%u build 0x%02x",
		 major, minor, patch, build);

	return 0;
}

static const struct acpi_device_id upboard_fpga_acpi_match[] = {
	{ "AANT0000", AANT0000_ID },
	{ "AANT0F00", AANT0F00_ID },
	{ "AANT0F01", AANT0F01_ID },
	{ "AANT0F02", AANT0F02_ID },
	{ "AANT0F03", AANT0F03_ID },
	{ "AANT0F04", AANT0F04_ID },
	{ }
};
MODULE_DEVICE_TABLE(acpi, upboard_fpga_acpi_match);

static int __init upboard_fpga_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct upboard_fpga *ddata;
	const struct acpi_device_id *id;
	static const struct regmap_config *cpld_config;
	static const struct mfd_cell *cells;
	static size_t ncells;
	int ret;

	id = acpi_match_device(upboard_fpga_acpi_match, dev);
	if (!id)
		return -ENODEV;

	switch (id->driver_data) {
	case AANT0F00_ID:
		cpld_config = &upboard_up_regmap_config;
		cells = upboard_up_mfd_cells;
		ncells = ARRAY_SIZE(upboard_up_mfd_cells);
		break;
	case AANT0F01_ID:
		cpld_config = &upboard_up2_regmap_config;
		cells = upboard_up2_mfd_cells;
		ncells = ARRAY_SIZE(upboard_up2_mfd_cells);
		break;
	case AANT0F02_ID:
		cpld_config = &upboard_up_regmap_config;
		cells = upboard_up_mfd_cells;
		ncells = ARRAY_SIZE(upboard_up_mfd_cells);
		break;
	case AANT0F03_ID:
		cpld_config = &upboard_up2_regmap_config;
		cells = upboard_up_mfd_cells;
		ncells = ARRAY_SIZE(upboard_up_mfd_cells);
		break;
	case AANT0F04_ID:
		cpld_config = &upboard_up_regmap_config;
		cells = upboard_up_mfd_cells;
		ncells = ARRAY_SIZE(upboard_up_mfd_cells);
		break;
	case AANT0000_ID:
	default:
		cpld_config = &upboard_up_regmap_config;
		cells = upboard_pinctrl_cells;
		ncells = ARRAY_SIZE(upboard_pinctrl_cells);
		break;
	}

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);
	ddata->dev = dev;
	ddata->regmap = devm_regmap_init(dev, NULL, ddata, cpld_config);
	if (IS_ERR(ddata->regmap))
		return PTR_ERR(ddata->regmap);

	ret = upboard_cpld_gpio_init(ddata);
	if (ret) {
		/* Not FPGA firmware, abort FPGA GPIO initialize process */
		dev_warn(dev, "Failed to initialize CPLD common GPIOs: %d", ret);
	} else {
		upboard_fpga_verify_device(ddata);
	}

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				    cells,
				    ncells,
				    NULL, 0, NULL);
}

static struct platform_driver upboard_fpga_driver = {
	.driver = {
		.name = "upboard-cpld",
		.acpi_match_table = upboard_fpga_acpi_match,
	},
};
module_platform_driver_probe(upboard_fpga_driver, upboard_fpga_probe);

MODULE_AUTHOR("Gary Wang <garywang@aaeon.com.tw>");
MODULE_DESCRIPTION("UP Board CPLD/FPGA driver");
MODULE_LICENSE("GPL v2");
