// SPDX-License-Identifier: GPL-2.0-only
/*
 * UP Board for control CPLD/FPGA to
 * provide more GPIO driving power also provide CPLD LEDs and pin mux function
 * recognize HID AANT0F00 ~ AAANT0F04 in ACPI name space
 *
 * Copyright (c) AAEON. All rights reserved.
 *
 * Author: Gary Wang <garywang@aaeon.com.tw>
 *
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

struct upboard_fpga_data {
	const struct regmap_config *regmap_config;
	const struct mfd_cell *cells;
	size_t ncells;
};

static struct gpio_led_platform_data pdata;

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
static int upboard_fpga_read(void *context, unsigned int reg, unsigned int *val)
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
static int upboard_fpga_write(void *context, unsigned int reg, unsigned int val)
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
	.reg_read = upboard_fpga_read,
	.reg_write = upboard_fpga_write,
	.fast_io = false,
	.cache_type = REGCACHE_NONE,
	.rd_table = &upboard_up_readable_table,
	.wr_table = &upboard_up_writable_table,
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
		       sizeof(*upboard_up_led_data), 1),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up_led_data[1],
		       sizeof(*upboard_up_led_data), 2),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up_led_data[2],
		       sizeof(*upboard_up_led_data), 3),
};

/* UP Squared 6000 EHL board */

static const struct upboard_fpga_data upboard_up_fpga_data = {
	.regmap_config = &upboard_up_regmap_config,
	.cells = upboard_up_mfd_cells,
	.ncells = ARRAY_SIZE(upboard_up_mfd_cells),
};

static const struct mfd_cell upboard_pinctrl_cells[] = {
	MFD_CELL_NAME("upboard-pinctrl"),
};

static const struct upboard_fpga_data upboard_pinctrl_data = {
	.regmap_config = &upboard_up_regmap_config,
	.cells = upboard_pinctrl_cells,
	.ncells = ARRAY_SIZE(upboard_pinctrl_cells),
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
	.reg_read = upboard_fpga_read,
	.reg_write = upboard_fpga_write,
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
		       sizeof(*upboard_up2_led_data), 1),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[1],
		       sizeof(*upboard_up2_led_data), 2),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[2],
		       sizeof(*upboard_up2_led_data), 3),
	MFD_CELL_BASIC("upboard-led", NULL, &upboard_up2_led_data[3],
		       sizeof(*upboard_up2_led_data), 4),
};

static const struct upboard_fpga_data upboard_up2_fpga_data = {
	.regmap_config = &upboard_up2_regmap_config,
	.cells = upboard_up2_mfd_cells,
	.ncells = ARRAY_SIZE(upboard_up2_mfd_cells),
};

/* UP-CRST02 carrier board for UP Core */

/* same MAX10 config as UP2, but same LED cells as UP1 */
static const struct upboard_fpga_data upboard_upcore_crst02_fpga_data = {
	.regmap_config = &upboard_up2_regmap_config,
	.cells = upboard_up_mfd_cells,
	.ncells = ARRAY_SIZE(upboard_up_mfd_cells),
};

static int __init upboard_fpga_gpio_init(struct upboard_fpga *fpga)
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

static const struct mfd_cell upboard_gpio_led_cells[] = {
	MFD_CELL_BASIC("leds-gpio", NULL,
		       &pdata,
		       sizeof(pdata), 0)
};

static int upboard_led_gpio_register(struct upboard_fpga *fpga)
{
	struct gpio_led blue_led, yellow_led, green_led, red_led;
	struct gpio_desc *desc;
	static struct gpio_led leds[4];
	int num_leds = 0;
	int ret;

	desc = devm_gpiod_get(fpga->dev, "blue", GPIOD_OUT_LOW);
	if (!IS_ERR(desc)) {
		blue_led.name = "upboard:blue:";
		blue_led.gpio = desc_to_gpio(desc);
		blue_led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
		leds[num_leds++] = blue_led;
		devm_gpiod_put(fpga->dev, desc);
	}

	desc = devm_gpiod_get(fpga->dev, "yellow", GPIOD_OUT_LOW);
	if (!IS_ERR(desc)) {
		yellow_led.name = "upboard:yellow:";
		yellow_led.gpio = desc_to_gpio(desc);
		yellow_led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
		leds[num_leds++] = yellow_led;
		devm_gpiod_put(fpga->dev, desc);
	}

	desc = devm_gpiod_get(fpga->dev, "green", GPIOD_OUT_LOW);
	if (!IS_ERR(desc)) {
		green_led.name = "upboard:green:";
		green_led.gpio = desc_to_gpio(desc);
		green_led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
		leds[num_leds++] = green_led;
		devm_gpiod_put(fpga->dev, desc);
	}

	desc = devm_gpiod_get(fpga->dev, "red", GPIOD_OUT_LOW);
	if (!IS_ERR(desc)) {
		red_led.name = "upboard:red:";
		red_led.gpio = desc_to_gpio(desc);
		red_led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
		leds[num_leds++] = red_led;
		devm_gpiod_put(fpga->dev, desc);
	}

	/* No optional LEDs defined */
	if (num_leds == 0)
		return 0;

	pdata.num_leds = num_leds;
	pdata.leds = leds;

	ret = devm_mfd_add_devices(fpga->dev, PLATFORM_DEVID_AUTO,
				   upboard_gpio_led_cells,
				   ARRAY_SIZE(upboard_gpio_led_cells),
				   NULL, 0, NULL);
	if (ret) {
		dev_err(fpga->dev, "Failed to add GPIO LEDs, %d", ret);
		return ret;
	}

	return 0;
}

/*
 * --------------------------------------       ------------
 * |    Intel SOC,1.8V                   | --- |ADC Chip   |  native driver
 * | GPIO/I2C/SPI/UART/PWM               |     |SPI/I2C    |
 * --------------------------------------      -------------
 *            |                                     |
 * ----------------------------------------------------------
 * |        CPLD/FPGA Driver                                |   upboard-fpga CPLD control driver
 * |   provide more GPIO driving power                      |   register leds-upboard
 * |        HAT 40 pin mux function                         |   register pinctrl-upboard
 * ---------------------------------------------------------
 *    |                                      |
 * ----------   -------------------------------------------
 * |3 or 4|     |    HAT 40 pins, 3.3V                    |   leds-upboard
 * | Leds |     |GPIO/ADC/I2C/SPI/UART/PWM                |  pinctrl-upboard
 * ----------   -------------------------------------------
 */
static const struct acpi_device_id upboard_fpga_acpi_match[] = {
	{ "AANT0000", (kernel_ulong_t)&upboard_pinctrl_data },
	{ "AANT0F00", (kernel_ulong_t)&upboard_up_fpga_data },
	{ "AANT0F01", (kernel_ulong_t)&upboard_up2_fpga_data },
	{ "AANT0F02", (kernel_ulong_t)&upboard_up_fpga_data },
	{ "AANT0F03", (kernel_ulong_t)&upboard_upcore_crst02_fpga_data },
	{ "AANT0F04", (kernel_ulong_t)&upboard_up_fpga_data },
	{ }
};
MODULE_DEVICE_TABLE(acpi, upboard_fpga_acpi_match);

static int __init upboard_fpga_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct upboard_fpga *ddata;
	const struct acpi_device_id *id;
	const struct upboard_fpga_data *fpga_data;
	int ret;

	id = acpi_match_device(upboard_fpga_acpi_match, dev);
	if (!id)
		return -ENODEV;

	fpga_data = (const struct upboard_fpga_data *) id->driver_data;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);
	ddata->dev = dev;

	ddata->regmap = devm_regmap_init(dev, NULL, ddata, fpga_data->regmap_config);
	if (IS_ERR(ddata->regmap))
		return PTR_ERR(ddata->regmap);

	ddata->regmap_config = fpga_data->regmap_config;

	ret = upboard_fpga_gpio_init(ddata);
	if (ret) {
		/* Not FPGA firmware, abort FPGA GPIO initialize process */
		dev_warn(dev, "Failed to initialize FPGA common GPIOs: %d", ret);
	} else {
		upboard_fpga_verify_device(ddata);
	}

	ret = upboard_led_gpio_register(ddata);
	if (ret) {
		/* LEDs are optional. */
		dev_warn(dev, "Failed to register LEDs: %d", ret);
	}

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				    fpga_data->cells,
				    fpga_data->ncells,
				    NULL, 0, NULL);
}

static struct platform_driver upboard_fpga_driver = {
	.driver = {
		.name = "upboard-fpga",
		.acpi_match_table = upboard_fpga_acpi_match,
	},
};
module_platform_driver_probe(upboard_fpga_driver, upboard_fpga_probe);

MODULE_AUTHOR("Gary Wang <garywang@aaeon.com.tw>");
MODULE_DESCRIPTION("UP Board CPLD/FPGA driver");
MODULE_LICENSE("GPL v2");
