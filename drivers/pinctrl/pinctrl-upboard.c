// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * UP board pin control driver.
 *
 * FPGA provides more GPIO driving power, LEDS and pin mux function.
 *
 * Copyright (c) AAEON. All rights reserved.
 * Copyright (C) 2024 Bootlin
 *
 * Author: Gary Wang <garywang@aaeon.com.tw>
 * Author: Thomas Richard <thomas.richard@bootlin.com>
 */

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/mfd/upboard-fpga.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#include "core.h"
#include "pinmux.h"

enum upboard_pin_mode {
	UPBOARD_PIN_MODE_FUNCTION = 1,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_DISABLED,
};

struct upboard_pin {
	struct regmap_field *funcbit;
	struct regmap_field *enbit;
	struct regmap_field *dirbit;
};

struct upboard_pingroup {
	struct pingroup grp;
	enum upboard_pin_mode mode;
	const enum upboard_pin_mode *modes;
};

struct upboard_pinctrl_data {
	const struct upboard_pingroup *groups;
	size_t ngroups;
	const struct pinfunction *funcs;
	size_t nfuncs;
	const struct pinctrl_map *maps;
	size_t nmaps;
	const unsigned int *pin_header;
	size_t ngpio;
};

struct upboard_pinctrl {
	struct gpio_chip chip;
	struct device *dev;
	struct pinctrl_dev *pctldev;
	const struct upboard_pinctrl_data *pctrl_data;
	struct gpio_pin_range pin_range;
	struct upboard_pin *pins;
	struct gpio_desc **gpio;
};

enum upboard_func0_fpgabit {
	UPBOARD_FUNC_I2C0_EN = 8,
	UPBOARD_FUNC_I2C1_EN = 9,
	UPBOARD_FUNC_CEC0_EN = 12,
	UPBOARD_FUNC_ADC0_EN = 14,
};

static const struct reg_field upboard_i2c0_reg =
	REG_FIELD(UPBOARD_REG_FUNC_EN0, UPBOARD_FUNC_I2C0_EN, UPBOARD_FUNC_I2C0_EN);

static const struct reg_field upboard_i2c1_reg =
	REG_FIELD(UPBOARD_REG_FUNC_EN0, UPBOARD_FUNC_I2C1_EN, UPBOARD_FUNC_I2C1_EN);

static const struct reg_field upboard_adc0_reg =
	REG_FIELD(UPBOARD_REG_FUNC_EN0, UPBOARD_FUNC_ADC0_EN, UPBOARD_FUNC_ADC0_EN);

#define UPBOARD_UP_BIT_TO_PIN(bit) UPBOARD_UP_BIT_##bit

#define UPBOARD_UP_PIN_NAME(id)					\
	{							\
		.number = UPBOARD_UP_BIT_##id,			\
		.name = #id,					\
	}

#define UPBOARD_UP_PIN_MUX(bit, data)				\
	{							\
		.number = UPBOARD_UP_BIT_##bit,			\
		.name = "PINMUX_"#bit,				\
		.drv_data = (void *)(data),			\
	}

#define UPBOARD_UP_PIN_FUNC(id, data)				\
	{							\
		.number = UPBOARD_UP_BIT_##id,			\
		.name = #id,					\
		.drv_data = (void *)(data),			\
	}

enum upboard_up_fpgabit {
	UPBOARD_UP_BIT_I2C1_SDA,
	UPBOARD_UP_BIT_I2C1_SCL,
	UPBOARD_UP_BIT_ADC0,
	UPBOARD_UP_BIT_UART1_RTS,
	UPBOARD_UP_BIT_GPIO27,
	UPBOARD_UP_BIT_GPIO22,
	UPBOARD_UP_BIT_SPI_MOSI,
	UPBOARD_UP_BIT_SPI_MISO,
	UPBOARD_UP_BIT_SPI_CLK,
	UPBOARD_UP_BIT_I2C0_SDA,
	UPBOARD_UP_BIT_GPIO5,
	UPBOARD_UP_BIT_GPIO6,
	UPBOARD_UP_BIT_PWM1,
	UPBOARD_UP_BIT_I2S_FRM,
	UPBOARD_UP_BIT_GPIO26,
	UPBOARD_UP_BIT_UART1_TX,
	UPBOARD_UP_BIT_UART1_RX,
	UPBOARD_UP_BIT_I2S_CLK,
	UPBOARD_UP_BIT_GPIO23,
	UPBOARD_UP_BIT_GPIO24,
	UPBOARD_UP_BIT_GPIO25,
	UPBOARD_UP_BIT_SPI_CS0,
	UPBOARD_UP_BIT_SPI_CS1,
	UPBOARD_UP_BIT_I2C0_SCL,
	UPBOARD_UP_BIT_PWM0,
	UPBOARD_UP_BIT_UART1_CTS,
	UPBOARD_UP_BIT_I2S_DIN,
	UPBOARD_UP_BIT_I2S_DOUT,
};

static const struct pinctrl_pin_desc upboard_up_pins[] = {
	UPBOARD_UP_PIN_FUNC(I2C1_SDA, &upboard_i2c1_reg),
	UPBOARD_UP_PIN_FUNC(I2C1_SCL, &upboard_i2c1_reg),
	UPBOARD_UP_PIN_FUNC(ADC0, &upboard_adc0_reg),
	UPBOARD_UP_PIN_NAME(UART1_RTS),
	UPBOARD_UP_PIN_NAME(GPIO27),
	UPBOARD_UP_PIN_NAME(GPIO22),
	UPBOARD_UP_PIN_NAME(SPI_MOSI),
	UPBOARD_UP_PIN_NAME(SPI_MISO),
	UPBOARD_UP_PIN_NAME(SPI_CLK),
	UPBOARD_UP_PIN_FUNC(I2C0_SDA, &upboard_i2c0_reg),
	UPBOARD_UP_PIN_NAME(GPIO5),
	UPBOARD_UP_PIN_NAME(GPIO6),
	UPBOARD_UP_PIN_NAME(PWM1),
	UPBOARD_UP_PIN_NAME(I2S_FRM),
	UPBOARD_UP_PIN_NAME(GPIO26),
	UPBOARD_UP_PIN_NAME(UART1_TX),
	UPBOARD_UP_PIN_NAME(UART1_RX),
	UPBOARD_UP_PIN_NAME(I2S_CLK),
	UPBOARD_UP_PIN_NAME(GPIO23),
	UPBOARD_UP_PIN_NAME(GPIO24),
	UPBOARD_UP_PIN_NAME(GPIO25),
	UPBOARD_UP_PIN_NAME(SPI_CS0),
	UPBOARD_UP_PIN_NAME(SPI_CS1),
	UPBOARD_UP_PIN_FUNC(I2C0_SCL, &upboard_i2c0_reg),
	UPBOARD_UP_PIN_NAME(PWM0),
	UPBOARD_UP_PIN_NAME(UART1_CTS),
	UPBOARD_UP_PIN_NAME(I2S_DIN),
	UPBOARD_UP_PIN_NAME(I2S_DOUT),
};

static const unsigned int upboard_up_pin_header[] = {
	UPBOARD_UP_BIT_TO_PIN(I2C0_SDA),
	UPBOARD_UP_BIT_TO_PIN(I2C0_SCL),
	UPBOARD_UP_BIT_TO_PIN(I2C1_SDA),
	UPBOARD_UP_BIT_TO_PIN(I2C1_SCL),
	UPBOARD_UP_BIT_TO_PIN(ADC0),
	UPBOARD_UP_BIT_TO_PIN(GPIO5),
	UPBOARD_UP_BIT_TO_PIN(GPIO6),
	UPBOARD_UP_BIT_TO_PIN(SPI_CS1),
	UPBOARD_UP_BIT_TO_PIN(SPI_CS0),
	UPBOARD_UP_BIT_TO_PIN(SPI_MISO),
	UPBOARD_UP_BIT_TO_PIN(SPI_MOSI),
	UPBOARD_UP_BIT_TO_PIN(SPI_CLK),
	UPBOARD_UP_BIT_TO_PIN(PWM0),
	UPBOARD_UP_BIT_TO_PIN(PWM1),
	UPBOARD_UP_BIT_TO_PIN(UART1_TX),
	UPBOARD_UP_BIT_TO_PIN(UART1_RX),
	UPBOARD_UP_BIT_TO_PIN(UART1_CTS),
	UPBOARD_UP_BIT_TO_PIN(UART1_RTS),
	UPBOARD_UP_BIT_TO_PIN(I2S_CLK),
	UPBOARD_UP_BIT_TO_PIN(I2S_FRM),
	UPBOARD_UP_BIT_TO_PIN(I2S_DIN),
	UPBOARD_UP_BIT_TO_PIN(I2S_DOUT),
	UPBOARD_UP_BIT_TO_PIN(GPIO22),
	UPBOARD_UP_BIT_TO_PIN(GPIO23),
	UPBOARD_UP_BIT_TO_PIN(GPIO24),
	UPBOARD_UP_BIT_TO_PIN(GPIO25),
	UPBOARD_UP_BIT_TO_PIN(GPIO26),
	UPBOARD_UP_BIT_TO_PIN(GPIO27),
};

static const unsigned int upboard_up_uart1_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(UART1_TX),
	UPBOARD_UP_BIT_TO_PIN(UART1_RX),
	UPBOARD_UP_BIT_TO_PIN(UART1_RTS),
	UPBOARD_UP_BIT_TO_PIN(UART1_CTS),
};

static const enum upboard_pin_mode upboard_up_uart1_modes[] = {
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
};

static_assert(ARRAY_SIZE(upboard_up_uart1_modes) ==
	      ARRAY_SIZE(upboard_up_uart1_pins));

static const unsigned int upboard_up_i2c0_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(I2C0_SCL),
	UPBOARD_UP_BIT_TO_PIN(I2C0_SDA),
};

static const unsigned int upboard_up_i2c1_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(I2C1_SCL),
	UPBOARD_UP_BIT_TO_PIN(I2C1_SDA),
};

static const unsigned int upboard_up_spi2_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(SPI_MOSI),
	UPBOARD_UP_BIT_TO_PIN(SPI_MISO),
	UPBOARD_UP_BIT_TO_PIN(SPI_CLK),
	UPBOARD_UP_BIT_TO_PIN(SPI_CS0),
	UPBOARD_UP_BIT_TO_PIN(SPI_CS1),
};

static const enum upboard_pin_mode upboard_up_spi2_modes[] = {
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_OUT,
};

static_assert(ARRAY_SIZE(upboard_up_spi2_modes) ==
	      ARRAY_SIZE(upboard_up_spi2_pins));

static const unsigned int upboard_up_i2s0_pins[]  = {
	UPBOARD_UP_BIT_TO_PIN(I2S_FRM),
	UPBOARD_UP_BIT_TO_PIN(I2S_CLK),
	UPBOARD_UP_BIT_TO_PIN(I2S_DIN),
	UPBOARD_UP_BIT_TO_PIN(I2S_DOUT),
};

static const enum upboard_pin_mode upboard_up_i2s0_modes[] = {
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT,
};

static_assert(ARRAY_SIZE(upboard_up_i2s0_pins) ==
	      ARRAY_SIZE(upboard_up_i2s0_modes));

static const unsigned int upboard_up_pwm0_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(PWM0),
};

static const unsigned int upboard_up_pwm1_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(PWM1),
};

static const unsigned int upboard_up_adc0_pins[] = {
	UPBOARD_UP_BIT_TO_PIN(ADC0),
};

#define UPBOARD_PINGROUP_MODE(_grp, _pins, _mode)			\
{									\
	.grp = PINCTRL_PINGROUP(_grp, _pins, ARRAY_SIZE(_pins)),	\
	.mode = _mode,							\
}

#define UPBOARD_PINGROUP_MODES(_grp, _pins, _modes)			\
{									\
	.grp = PINCTRL_PINGROUP(_grp, _pins, ARRAY_SIZE(_pins)),	\
	.modes = _modes,							\
}

static const struct upboard_pingroup upboard_up_pin_groups[] = {
	UPBOARD_PINGROUP_MODES("uart1_grp", upboard_up_uart1_pins, upboard_up_uart1_modes),
	UPBOARD_PINGROUP_MODE("i2c0_grp", upboard_up_i2c0_pins, UPBOARD_PIN_MODE_GPIO_OUT),
	UPBOARD_PINGROUP_MODE("i2c1_grp", upboard_up_i2c1_pins, UPBOARD_PIN_MODE_GPIO_OUT),
	UPBOARD_PINGROUP_MODES("spi2_grp", upboard_up_spi2_pins, &upboard_up_spi2_modes[0]),
	UPBOARD_PINGROUP_MODES("i2s0_grp", upboard_up_i2s0_pins, &upboard_up_i2s0_modes[0]),
	UPBOARD_PINGROUP_MODE("pwm0_grp", upboard_up_pwm0_pins, UPBOARD_PIN_MODE_GPIO_OUT),
	UPBOARD_PINGROUP_MODE("pwm1_grp", upboard_up_pwm1_pins, UPBOARD_PIN_MODE_GPIO_OUT),
	UPBOARD_PINGROUP_MODE("adc0_grp", upboard_up_adc0_pins, UPBOARD_PIN_MODE_GPIO_IN),
};

static const char * const upboard_up_uart1_groups[] = { "uart1_grp" };
static const char * const upboard_up_i2c0_groups[]  = { "i2c0_grp" };
static const char * const upboard_up_i2c1_groups[]  = { "i2c1_grp" };
static const char * const upboard_up_spi2_groups[]  = { "spi2_grp" };
static const char * const upboard_up_i2s0_groups[]  = { "i2s0_grp" };
static const char * const upboard_up_pwm0_groups[]  = { "pwm0_grp" };
static const char * const upboard_up_pwm1_groups[]  = { "pwm1_grp" };
static const char * const upboard_up_adc0_groups[]  = { "adc0_grp" };

#define UPBOARD_FUNCTION(func, groups)	PINCTRL_PINFUNCTION(func, groups, ARRAY_SIZE(groups))

static const struct pinfunction upboard_up_pin_functions[] = {
	UPBOARD_FUNCTION("uart1", upboard_up_uart1_groups),
	UPBOARD_FUNCTION("i2c0", upboard_up_i2c0_groups),
	UPBOARD_FUNCTION("i2c1", upboard_up_i2c1_groups),
	UPBOARD_FUNCTION("spi2", upboard_up_spi2_groups),
	UPBOARD_FUNCTION("i2s0", upboard_up_i2s0_groups),
	UPBOARD_FUNCTION("pwm0", upboard_up_pwm0_groups),
	UPBOARD_FUNCTION("pwm1", upboard_up_pwm1_groups),
	UPBOARD_FUNCTION("adc0", upboard_up_adc0_groups),
};

static const struct pinctrl_map upboard_up_pin_mapping[] = {
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "uart1_grp", "uart1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "i2c0_grp", "i2c0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "i2c1_grp", "i2c1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "spi2_grp", "spi2"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "i2s0_grp", "i2s0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "pwm0_grp", "pwm0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "pwm1_grp", "pwm1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "adc0_grp", "adc0"),
};

static const struct upboard_pinctrl_data upboard_up_pinctrl_data = {
	.groups = &upboard_up_pin_groups[0],
	.ngroups = ARRAY_SIZE(upboard_up_pin_groups),
	.funcs = &upboard_up_pin_functions[0],
	.nfuncs = ARRAY_SIZE(upboard_up_pin_functions),
	.maps = &upboard_up_pin_mapping[0],
	.nmaps = ARRAY_SIZE(upboard_up_pin_mapping),
	.pin_header = &upboard_up_pin_header[0],
	.ngpio = ARRAY_SIZE(upboard_up_pin_header),
};

#define UPBOARD_UP2_BIT_TO_PIN(bit) UPBOARD_UP2_BIT_##bit

#define UPBOARD_UP2_PIN_NAME(id)					\
	{								\
		.number = UPBOARD_UP2_BIT_##id,				\
		.name = #id,						\
	}

#define UPBOARD_UP2_PIN_MUX(bit, data)					\
	{								\
		.number = UPBOARD_UP2_BIT_##bit,			\
		.name = "PINMUX_"#bit,					\
		.drv_data = (void *)(data),				\
	}

#define UPBOARD_UP2_PIN_FUNC(id, data)					\
	{								\
		.number = UPBOARD_UP2_BIT_##id,				\
		.name = #id,						\
		.drv_data = (void *)(data),				\
	}

enum upboard_up2_fpgabit {
	UPBOARD_UP2_BIT_UART1_TXD,
	UPBOARD_UP2_BIT_UART1_RXD,
	UPBOARD_UP2_BIT_UART1_RTS,
	UPBOARD_UP2_BIT_UART1_CTS,
	UPBOARD_UP2_BIT_GPIO3_ADC0,
	UPBOARD_UP2_BIT_GPIO5_ADC2,
	UPBOARD_UP2_BIT_GPIO6_ADC3,
	UPBOARD_UP2_BIT_GPIO11,
	UPBOARD_UP2_BIT_EXHAT_LVDS1n,
	UPBOARD_UP2_BIT_EXHAT_LVDS1p,
	UPBOARD_UP2_BIT_SPI2_TXD,
	UPBOARD_UP2_BIT_SPI2_RXD,
	UPBOARD_UP2_BIT_SPI2_FS1,
	UPBOARD_UP2_BIT_SPI2_FS0,
	UPBOARD_UP2_BIT_SPI2_CLK,
	UPBOARD_UP2_BIT_SPI1_TXD,
	UPBOARD_UP2_BIT_SPI1_RXD,
	UPBOARD_UP2_BIT_SPI1_FS1,
	UPBOARD_UP2_BIT_SPI1_FS0,
	UPBOARD_UP2_BIT_SPI1_CLK,
	UPBOARD_UP2_BIT_I2C0_SCL,
	UPBOARD_UP2_BIT_I2C0_SDA,
	UPBOARD_UP2_BIT_I2C1_SCL,
	UPBOARD_UP2_BIT_I2C1_SDA,
	UPBOARD_UP2_BIT_PWM1,
	UPBOARD_UP2_BIT_PWM0,
	UPBOARD_UP2_BIT_EXHAT_LVDS0n,
	UPBOARD_UP2_BIT_EXHAT_LVDS0p,
	UPBOARD_UP2_BIT_GPIO24,
	UPBOARD_UP2_BIT_GPIO10,
	UPBOARD_UP2_BIT_GPIO2,
	UPBOARD_UP2_BIT_GPIO1,
	UPBOARD_UP2_BIT_EXHAT_LVDS3n,
	UPBOARD_UP2_BIT_EXHAT_LVDS3p,
	UPBOARD_UP2_BIT_EXHAT_LVDS4n,
	UPBOARD_UP2_BIT_EXHAT_LVDS4p,
	UPBOARD_UP2_BIT_EXHAT_LVDS5n,
	UPBOARD_UP2_BIT_EXHAT_LVDS5p,
	UPBOARD_UP2_BIT_I2S_SDO,
	UPBOARD_UP2_BIT_I2S_SDI,
	UPBOARD_UP2_BIT_I2S_WS_SYNC,
	UPBOARD_UP2_BIT_I2S_BCLK,
	UPBOARD_UP2_BIT_EXHAT_LVDS6n,
	UPBOARD_UP2_BIT_EXHAT_LVDS6p,
	UPBOARD_UP2_BIT_EXHAT_LVDS7n,
	UPBOARD_UP2_BIT_EXHAT_LVDS7p,
	UPBOARD_UP2_BIT_EXHAT_LVDS2n,
	UPBOARD_UP2_BIT_EXHAT_LVDS2p,
};

static const struct pinctrl_pin_desc upboard_up2_pins[] = {
	UPBOARD_UP2_PIN_NAME(UART1_TXD),
	UPBOARD_UP2_PIN_NAME(UART1_RXD),
	UPBOARD_UP2_PIN_NAME(UART1_RTS),
	UPBOARD_UP2_PIN_NAME(UART1_CTS),
	UPBOARD_UP2_PIN_NAME(GPIO3_ADC0),
	UPBOARD_UP2_PIN_NAME(GPIO5_ADC2),
	UPBOARD_UP2_PIN_NAME(GPIO6_ADC3),
	UPBOARD_UP2_PIN_NAME(GPIO11),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS1n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS1p),
	UPBOARD_UP2_PIN_NAME(SPI2_TXD),
	UPBOARD_UP2_PIN_NAME(SPI2_RXD),
	UPBOARD_UP2_PIN_NAME(SPI2_FS1),
	UPBOARD_UP2_PIN_NAME(SPI2_FS0),
	UPBOARD_UP2_PIN_NAME(SPI2_CLK),
	UPBOARD_UP2_PIN_NAME(SPI1_TXD),
	UPBOARD_UP2_PIN_NAME(SPI1_RXD),
	UPBOARD_UP2_PIN_NAME(SPI1_FS1),
	UPBOARD_UP2_PIN_NAME(SPI1_FS0),
	UPBOARD_UP2_PIN_NAME(SPI1_CLK),
	UPBOARD_UP2_PIN_MUX(I2C0_SCL, &upboard_i2c0_reg),
	UPBOARD_UP2_PIN_MUX(I2C0_SDA, &upboard_i2c0_reg),
	UPBOARD_UP2_PIN_MUX(I2C1_SCL, &upboard_i2c1_reg),
	UPBOARD_UP2_PIN_MUX(I2C1_SDA, &upboard_i2c1_reg),
	UPBOARD_UP2_PIN_NAME(PWM1),
	UPBOARD_UP2_PIN_NAME(PWM0),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS0n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS0p),
	UPBOARD_UP2_PIN_MUX(GPIO24, &upboard_i2c0_reg),
	UPBOARD_UP2_PIN_MUX(GPIO10, &upboard_i2c0_reg),
	UPBOARD_UP2_PIN_MUX(GPIO2, &upboard_i2c1_reg),
	UPBOARD_UP2_PIN_MUX(GPIO1, &upboard_i2c1_reg),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS3n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS3p),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS4n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS4p),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS5n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS5p),
	UPBOARD_UP2_PIN_NAME(I2S_SDO),
	UPBOARD_UP2_PIN_NAME(I2S_SDI),
	UPBOARD_UP2_PIN_NAME(I2S_WS_SYNC),
	UPBOARD_UP2_PIN_NAME(I2S_BCLK),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS6n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS6p),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS7n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS7p),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS2n),
	UPBOARD_UP2_PIN_NAME(EXHAT_LVDS2p),
};

static const unsigned int upboard_up2_pin_header[] = {
	UPBOARD_UP2_BIT_TO_PIN(GPIO10),
	UPBOARD_UP2_BIT_TO_PIN(GPIO24),
	UPBOARD_UP2_BIT_TO_PIN(GPIO1),
	UPBOARD_UP2_BIT_TO_PIN(GPIO2),
	UPBOARD_UP2_BIT_TO_PIN(GPIO3_ADC0),
	UPBOARD_UP2_BIT_TO_PIN(GPIO11),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_CLK),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_FS1),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_FS0),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_RXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_TXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_CLK),
	UPBOARD_UP2_BIT_TO_PIN(PWM0),
	UPBOARD_UP2_BIT_TO_PIN(PWM1),
	UPBOARD_UP2_BIT_TO_PIN(UART1_TXD),
	UPBOARD_UP2_BIT_TO_PIN(UART1_RXD),
	UPBOARD_UP2_BIT_TO_PIN(UART1_CTS),
	UPBOARD_UP2_BIT_TO_PIN(UART1_RTS),
	UPBOARD_UP2_BIT_TO_PIN(I2S_BCLK),
	UPBOARD_UP2_BIT_TO_PIN(I2S_WS_SYNC),
	UPBOARD_UP2_BIT_TO_PIN(I2S_SDI),
	UPBOARD_UP2_BIT_TO_PIN(I2S_SDO),
	UPBOARD_UP2_BIT_TO_PIN(GPIO6_ADC3),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_FS1),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_RXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_TXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_FS0),
	UPBOARD_UP2_BIT_TO_PIN(GPIO5_ADC2),
};

static const unsigned int upboard_up2_uart1_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(UART1_TXD),
	UPBOARD_UP2_BIT_TO_PIN(UART1_RXD),
	UPBOARD_UP2_BIT_TO_PIN(UART1_RTS),
	UPBOARD_UP2_BIT_TO_PIN(UART1_CTS),
};

static const enum upboard_pin_mode upboard_up2_uart1_modes[] = {
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN
};

static_assert(ARRAY_SIZE(upboard_up2_uart1_modes) ==
	      ARRAY_SIZE(upboard_up2_uart1_pins));

static const unsigned int upboard_up2_i2c0_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(I2C0_SCL),
	UPBOARD_UP2_BIT_TO_PIN(I2C0_SDA),
	UPBOARD_UP2_BIT_TO_PIN(GPIO24),
	UPBOARD_UP2_BIT_TO_PIN(GPIO10),
};

static const unsigned int upboard_up2_i2c1_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(I2C1_SCL),
	UPBOARD_UP2_BIT_TO_PIN(I2C1_SDA),
	UPBOARD_UP2_BIT_TO_PIN(GPIO2),
	UPBOARD_UP2_BIT_TO_PIN(GPIO1),
};

static const unsigned int upboard_up2_spi1_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(SPI1_TXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_RXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_FS1),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_FS0),
	UPBOARD_UP2_BIT_TO_PIN(SPI1_CLK),
};

static const unsigned int upboard_up2_spi2_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(SPI2_TXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_RXD),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_FS1),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_FS0),
	UPBOARD_UP2_BIT_TO_PIN(SPI2_CLK),
};

static const enum upboard_pin_mode upboard_up2_spi_modes[] = {
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_OUT,
};

static_assert(ARRAY_SIZE(upboard_up2_spi_modes) ==
	      ARRAY_SIZE(upboard_up2_spi1_pins));

static_assert(ARRAY_SIZE(upboard_up2_spi_modes) ==
	      ARRAY_SIZE(upboard_up2_spi2_pins));

static const unsigned int upboard_up2_i2s0_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(I2S_BCLK),
	UPBOARD_UP2_BIT_TO_PIN(I2S_WS_SYNC),
	UPBOARD_UP2_BIT_TO_PIN(I2S_SDI),
	UPBOARD_UP2_BIT_TO_PIN(I2S_SDO),
};

static const enum upboard_pin_mode upboard_up2_i2s0_modes[] = {
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_OUT,
	UPBOARD_PIN_MODE_GPIO_IN,
	UPBOARD_PIN_MODE_GPIO_OUT
};

static_assert(ARRAY_SIZE(upboard_up2_i2s0_modes) ==
	      ARRAY_SIZE(upboard_up2_i2s0_pins));

static const unsigned int upboard_up2_pwm0_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(PWM0),
};

static const unsigned int upboard_up2_pwm1_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(PWM1),
};

static const unsigned int upboard_up2_adc0_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(GPIO3_ADC0),
};

static const unsigned int upboard_up2_adc2_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(GPIO5_ADC2),
};

static const unsigned int upboard_up2_adc3_pins[] = {
	UPBOARD_UP2_BIT_TO_PIN(GPIO6_ADC3),
};

static const struct upboard_pingroup upboard_up2_pin_groups[] = {
	UPBOARD_PINGROUP_MODES("uart1_grp", upboard_up2_uart1_pins, upboard_up2_uart1_modes),
	UPBOARD_PINGROUP_MODE("i2c0_grp", upboard_up2_i2c0_pins, UPBOARD_PIN_MODE_FUNCTION),
	UPBOARD_PINGROUP_MODE("i2c1_grp", upboard_up2_i2c1_pins, UPBOARD_PIN_MODE_FUNCTION),
	UPBOARD_PINGROUP_MODES("spi1_grp", upboard_up2_spi1_pins, upboard_up2_spi_modes),
	UPBOARD_PINGROUP_MODES("spi2_grp", upboard_up2_spi2_pins, upboard_up2_spi_modes),
	UPBOARD_PINGROUP_MODES("i2s0_grp", upboard_up2_i2s0_pins, upboard_up2_i2s0_modes),
	UPBOARD_PINGROUP_MODE("pwm0_grp", upboard_up2_pwm0_pins, UPBOARD_PIN_MODE_GPIO_OUT),
	UPBOARD_PINGROUP_MODE("pwm1_grp", upboard_up2_pwm1_pins, UPBOARD_PIN_MODE_GPIO_OUT),
	UPBOARD_PINGROUP_MODE("adc0_grp", upboard_up2_adc0_pins, UPBOARD_PIN_MODE_GPIO_IN),
	UPBOARD_PINGROUP_MODE("adc2_grp", upboard_up2_adc2_pins, UPBOARD_PIN_MODE_GPIO_IN),
	UPBOARD_PINGROUP_MODE("adc3_grp", upboard_up2_adc3_pins, UPBOARD_PIN_MODE_GPIO_IN),
};

static const char * const upboard_up2_uart1_groups[] = { "uart1_grp" };
static const char * const upboard_up2_i2c0_groups[]  = { "i2c0_grp" };
static const char * const upboard_up2_i2c1_groups[]  = { "i2c1_grp" };
static const char * const upboard_up2_spi1_groups[]  = { "spi1_grp" };
static const char * const upboard_up2_spi2_groups[]  = { "spi2_grp" };
static const char * const upboard_up2_i2s0_groups[]  = { "i2s0_grp" };
static const char * const upboard_up2_pwm0_groups[]  = { "pwm0_grp" };
static const char * const upboard_up2_pwm1_groups[]  = { "pwm1_grp" };
static const char * const upboard_up2_adc0_groups[]  = { "adc0_grp" };
static const char * const upboard_up2_adc2_groups[]  = { "adc2_grp" };
static const char * const upboard_up2_adc3_groups[]  = { "adc3_grp" };

static const struct pinfunction upboard_up2_pin_functions[] = {
	UPBOARD_FUNCTION("uart1", upboard_up2_uart1_groups),
	UPBOARD_FUNCTION("i2c0", upboard_up2_i2c0_groups),
	UPBOARD_FUNCTION("i2c1", upboard_up2_i2c1_groups),
	UPBOARD_FUNCTION("spi1", upboard_up2_spi1_groups),
	UPBOARD_FUNCTION("spi2", upboard_up2_spi2_groups),
	UPBOARD_FUNCTION("i2s0", upboard_up2_i2s0_groups),
	UPBOARD_FUNCTION("pwm0", upboard_up2_pwm0_groups),
	UPBOARD_FUNCTION("pwm1", upboard_up2_pwm1_groups),
	UPBOARD_FUNCTION("adc0", upboard_up2_adc0_groups),
	UPBOARD_FUNCTION("adc2", upboard_up2_adc2_groups),
	UPBOARD_FUNCTION("adc3", upboard_up2_adc3_groups),
};

static const struct pinctrl_map upboard_up2_pin_mapping[] = {
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "uart1_grp", "uart1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "i2c0_grp", "i2c0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "i2c1_grp", "i2c1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "spi1_grp", "spi1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "spi2_grp", "spi2"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "i2s0_grp", "i2s0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "pwm0_grp", "pwm0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "pwm1_grp", "pwm1"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "adc0_grp", "adc0"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "adc2_grp", "adc2"),
	PIN_MAP_MUX_GROUP_HOG_DEFAULT("upboard-pinctrl", "adc3_grp", "adc3"),
};

static const struct upboard_pinctrl_data upboard_up2_pinctrl_data = {
	.groups = &upboard_up2_pin_groups[0],
	.ngroups = ARRAY_SIZE(upboard_up2_pin_groups),
	.funcs = &upboard_up2_pin_functions[0],
	.nfuncs = ARRAY_SIZE(upboard_up2_pin_functions),
	.maps = &upboard_up2_pin_mapping[0],
	.nmaps = ARRAY_SIZE(upboard_up2_pin_mapping),
	.pin_header = &upboard_up2_pin_header[0],
	.ngpio = ARRAY_SIZE(upboard_up2_pin_header),
};

static int __upboard_pinctrl_gpio_request_enable(struct pinctrl_dev *pctldev,
						 unsigned int offset)
{
	struct upboard_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	struct upboard_pin *p = &pctrl->pins[offset];

	if (p->funcbit) {
		int ret = regmap_field_write(p->funcbit, 0);

		if (ret)
			return ret;
	}

	return regmap_field_write(p->enbit, 1);
}

static int upboard_pinctrl_gpio_request_enable(struct pinctrl_dev *pctldev,
					       struct pinctrl_gpio_range *range,
					       unsigned int offset)
{
	return __upboard_pinctrl_gpio_request_enable(pctldev, offset);
}

static void __upboard_pinctrl_gpio_disable_free(struct pinctrl_dev *pctldev, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	struct upboard_pin *p = &pctrl->pins[offset];

	regmap_field_write(p->enbit, 0);

	if (p->funcbit)
		regmap_field_write(p->funcbit, 1);
};

static void upboard_pinctrl_gpio_disable_free(struct pinctrl_dev *pctldev,
					      struct pinctrl_gpio_range *range, unsigned int offset)
{
	return __upboard_pinctrl_gpio_disable_free(pctldev, offset);
}

static int __upboard_pinctrl_gpio_set_direction(struct pinctrl_dev *pctldev,
						unsigned int offset, bool input)
{
	struct upboard_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	struct upboard_pin *p = &pctrl->pins[offset];

	return regmap_field_write(p->dirbit, input);
}

static int upboard_pinctrl_gpio_set_direction(struct pinctrl_dev *pctldev,
					      struct pinctrl_gpio_range *range,
					      unsigned int offset, bool input)
{
	return __upboard_pinctrl_gpio_set_direction(pctldev, offset, input);
}

static int upboard_pinctrl_set_mux(struct pinctrl_dev *pctldev, unsigned int func_selector,
				   unsigned int group_selector)
{
	struct upboard_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct upboard_pinctrl_data *pctrl_data = pctrl->pctrl_data;
	const struct upboard_pingroup *upgroups = pctrl_data->groups;
	struct group_desc *grp;
	int mode, ret, i;

	grp = pinctrl_generic_get_group(pctldev, group_selector);
	if (!grp)
		return -EINVAL;

	for (i = 0; i < grp->grp.npins; i++) {
		mode = upgroups[group_selector].mode ? upgroups[group_selector].mode :
			upgroups[group_selector].modes[i];

		if (mode == UPBOARD_PIN_MODE_FUNCTION) {
			__upboard_pinctrl_gpio_disable_free(pctldev, grp->grp.pins[i]);
			continue;
		}

		ret = __upboard_pinctrl_gpio_request_enable(pctldev, grp->grp.pins[i]);
		if (ret)
			return ret;

		ret = __upboard_pinctrl_gpio_set_direction(pctldev, grp->grp.pins[i],
							   mode == UPBOARD_PIN_MODE_GPIO_IN ?
							   true : false);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct pinmux_ops upboard_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = upboard_pinctrl_set_mux,
	.gpio_request_enable = upboard_pinctrl_gpio_request_enable,
	.gpio_disable_free = upboard_pinctrl_gpio_disable_free,
	.gpio_set_direction = upboard_pinctrl_gpio_set_direction,
};

static int upboard_pinctrl_pin_get_mode(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct upboard_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	struct upboard_pin *p = &pctrl->pins[pin];

	unsigned int val;
	int ret;

	if (p->funcbit) {
		ret = regmap_field_read(p->funcbit, &val);
		if (ret)
			return ret;
		if (val)
			return UPBOARD_PIN_MODE_FUNCTION;
	}

	ret = regmap_field_read(p->enbit, &val);
	if (ret)
		return ret;
	if (!val)
		return UPBOARD_PIN_MODE_DISABLED;

	ret = regmap_field_read(p->dirbit, &val);
	if (ret)
		return ret;

	return val ? UPBOARD_PIN_MODE_GPIO_IN : UPBOARD_PIN_MODE_GPIO_OUT;
}

static void upboard_pinctrl_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
				     unsigned int offset)
{
	int ret = upboard_pinctrl_pin_get_mode(pctldev, offset);

	if (ret == UPBOARD_PIN_MODE_FUNCTION)
		seq_puts(s, "mode function ");
	else if (ret == UPBOARD_PIN_MODE_DISABLED)
		seq_puts(s, "HIGH-Z");
	else
		seq_printf(s, "GPIO (%s) ", ret == UPBOARD_PIN_MODE_GPIO_IN ? "input" : "output");
}

static const struct pinctrl_ops upboard_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.pin_dbg_show = upboard_pinctrl_dbg_show,
};

static int upboard_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);
	unsigned int pin = pctrl->pctrl_data->pin_header[offset];
	int ret;

	ret = pinctrl_gpio_request(gc, offset);
	if (ret)
		return ret;

	pctrl->gpio[offset] = gpiod_get_index(pctrl->dev, "external", pin, 0);

	if (IS_ERR(pctrl->gpio[offset])) {
		pinctrl_gpio_free(gc, offset);
		return PTR_ERR(pctrl->gpio[offset]);
	}

	return 0;
}

static void upboard_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);

	gpiod_put(pctrl->gpio[offset]);

	pinctrl_gpio_free(gc, offset);
}

static int upboard_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);
	unsigned int pin = pctrl->pctrl_data->pin_header[offset];
	int mode;

	if (pctrl->gpio[offset])
		return gpiod_get_direction(pctrl->gpio[offset]);

	/*
	 * GPIO was not requested so SoC pin is probably not in GPIO mode.
	 * When a gpio_chip is registered, the core calls get_direction() for all lines.
	 * At this time, upboard_gpio_request() was not yet called, so the driver didn't
	 * request the corresponding SoC pin. So the SoC pin is probably in function (not in
	 * GPIO mode).
	 *
	 * To get the direction of the SoC pin, it shall be requested in GPIO mode.
	 * Once a SoC pin is set in GPIO mode, there is no way to set it back to its
	 * function mode.
	 * Instead of returning the SoC pin direction, the direction of the FPGA pin is
	 * returned (only for the get_direction() called during the gpio_chip registration).
	 */
	mode = upboard_pinctrl_pin_get_mode(pctrl->pctldev, pin);

	/* If the pin is in function mode or high-z, input direction is returned */
	if (mode < 0)
		return mode;
	if (mode == UPBOARD_PIN_MODE_GPIO_OUT)
		return GPIO_LINE_DIRECTION_OUT;
	else
		return GPIO_LINE_DIRECTION_IN;
}

static int upboard_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);

	return gpiod_get_value(pctrl->gpio[offset]);
}

static void upboard_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);

	gpiod_set_value(pctrl->gpio[offset], value);
}

static int upboard_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);
	int ret;

	ret = pinctrl_gpio_direction_input(gc, offset);
	if (ret)
		return ret;

	return gpiod_direction_input(pctrl->gpio[offset]);
}

static int upboard_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);
	int ret;

	ret = pinctrl_gpio_direction_output(gc, offset);
	if (ret)
		return ret;

	return gpiod_direction_output(pctrl->gpio[offset], value);
}

static int upboard_gpio_to_irq(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = container_of(gc, struct upboard_pinctrl, chip);

	return gpiod_to_irq(pctrl->gpio[offset]);
}

static int upboard_pinctrl_register_groups(struct upboard_pinctrl *pctrl)
{
	const struct upboard_pingroup *groups = pctrl->pctrl_data->groups;
	size_t ngroups = pctrl->pctrl_data->ngroups;
	int ret, i;

	for (i = 0; i < ngroups; i++) {
		ret = pinctrl_generic_add_group(pctrl->pctldev, groups[i].grp.name,
						groups[i].grp.pins, groups[i].grp.npins, pctrl);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int upboard_pinctrl_register_functions(struct upboard_pinctrl *pctrl)
{
	const struct pinfunction *funcs = pctrl->pctrl_data->funcs;
	size_t nfuncs = pctrl->pctrl_data->nfuncs;
	int ret, i;

	for (i = 0; i < nfuncs ; ++i) {
		ret = pinmux_generic_add_function(pctrl->pctldev, funcs[i].name, funcs[i].groups,
						  funcs[i].ngroups, NULL);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int upboard_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct upboard_fpga *fpga = dev_get_drvdata(dev->parent);
	struct pinctrl_desc *pctldesc;
	struct upboard_pinctrl *pctrl;
	struct upboard_pin *pins;
	struct gpio_chip *chip;
	int ret, i;

	pctldesc = devm_kzalloc(dev, sizeof(*pctldesc), GFP_KERNEL);
	if (!pctldesc)
		return -ENOMEM;

	pctrl = devm_kzalloc(dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	switch (fpga->fpga_data->type) {
	case UPBOARD_UP_FPGA:
		pctldesc->pins = upboard_up_pins;
		pctldesc->npins = ARRAY_SIZE(upboard_up_pins);
		pctrl->pctrl_data = &upboard_up_pinctrl_data;
		break;
	case UPBOARD_UP2_FPGA:
		pctldesc->pins = upboard_up2_pins;
		pctldesc->npins = ARRAY_SIZE(upboard_up2_pins);
		pctrl->pctrl_data = &upboard_up2_pinctrl_data;
		break;
	default:
		return dev_err_probe(dev, -ENODEV, "Unsupported device type %d\n",
				     fpga->fpga_data->type);
	}

	pctldesc->name = dev_name(dev);
	pctldesc->owner = THIS_MODULE;
	pctldesc->pctlops = &upboard_pinctrl_ops;
	pctldesc->pmxops = &upboard_pinmux_ops;

	pctrl->dev = dev;

	pins = devm_kcalloc(dev, pctldesc->npins, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;

	/* Initialize pins */
	for (i = 0; i < pctldesc->npins; i++) {
		const struct pinctrl_pin_desc *pin_desc = &pctldesc->pins[i];
		unsigned int regoff = pin_desc->number / UPBOARD_REGISTER_SIZE;
		unsigned int lsb = pin_desc->number % UPBOARD_REGISTER_SIZE;
		struct reg_field * const fld_func = pin_desc->drv_data;
		struct upboard_pin *pin = &pins[i];
		struct reg_field fldconf = {};

		if (fld_func) {
			pin->funcbit = devm_regmap_field_alloc(dev, fpga->regmap, *fld_func);

			if (IS_ERR(pin->funcbit))
				return PTR_ERR(pin->funcbit);
		}

		fldconf.reg = UPBOARD_REG_GPIO_EN0 + regoff;
		fldconf.lsb = lsb;
		fldconf.msb = lsb;
		pin->enbit = devm_regmap_field_alloc(dev, fpga->regmap, fldconf);
		if (IS_ERR(pin->enbit))
			return PTR_ERR(pin->enbit);

		fldconf.reg = UPBOARD_REG_GPIO_DIR0 + regoff;
		fldconf.lsb = lsb;
		fldconf.msb = lsb;
		pin->dirbit = devm_regmap_field_alloc(dev, fpga->regmap, fldconf);
		if (IS_ERR(pin->dirbit))
			return PTR_ERR(pin->dirbit);
	}

	pctrl->pins = pins;

	ret = pinctrl_register_mappings(pctrl->pctrl_data->maps, pctrl->pctrl_data->nmaps);

	ret = devm_pinctrl_register_and_init(dev, pctldesc, pctrl, &pctrl->pctldev);
	if (ret)
		return ret;

	ret = upboard_pinctrl_register_groups(pctrl);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register groups");

	ret = upboard_pinctrl_register_functions(pctrl);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register functions");

	ret = pinctrl_enable(pctrl->pctldev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable pinctrl");

	chip = &pctrl->chip;
	chip->owner = THIS_MODULE;
	chip->label = dev_name(&pdev->dev);
	chip->parent = &pdev->dev;
	chip->ngpio = pctrl->pctrl_data->ngpio;
	chip->base = -1;
	chip->request = upboard_gpio_request;
	chip->free = upboard_gpio_free;
	chip->get = upboard_gpio_get;
	chip->set = upboard_gpio_set;
	chip->get_direction = upboard_gpio_get_direction;
	chip->direction_input = upboard_gpio_direction_input;
	chip->direction_output = upboard_gpio_direction_output;
	chip->to_irq = upboard_gpio_to_irq;

	pctrl->gpio = devm_kcalloc(dev, pctrl->pctrl_data->ngpio, sizeof(*pctrl->gpio), GFP_KERNEL);
	if (!pctrl->gpio)
		return -ENOMEM;

	ret = devm_gpiochip_add_data(&pdev->dev, chip, pctrl);
	if (ret)
		return ret;

	ret = gpiochip_add_pinlist_range(chip, dev_name(dev), 0, pctrl->pctrl_data->pin_header,
					 pctrl->pctrl_data->ngpio);
	if (ret)
		gpiochip_remove(chip);

	return ret;
}

static struct platform_driver upboard_pinctrl_driver = {
	.driver = {
		.name = "upboard-pinctrl",
	},
	.probe = upboard_pinctrl_probe,
};

module_platform_driver(upboard_pinctrl_driver);

MODULE_AUTHOR("Gary Wang <garywang@aaeon.com.tw>");
MODULE_AUTHOR("Thomas Richard <thomas.richard@bootlin.com");
MODULE_DESCRIPTION("UP Board HAT pin controller driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:upboard-pinctrl");
