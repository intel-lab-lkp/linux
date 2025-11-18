/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Maxime Coquelin 2015
 * Copyright (C) STMicroelectronics 2017
 * Author:  Maxime Coquelin <mcoquelin.stm32@gmail.com>
 */
#ifndef __PINCTRL_STM32_H
#define __PINCTRL_STM32_H

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>

#define STM32_PIN_NO(x) ((x) << 8)
#define STM32_GET_PIN_NO(x) ((x) >> 8)
#define STM32_GET_PIN_FUNC(x) ((x) & 0xff)

#define STM32_PIN_GPIO		0
#define STM32_PIN_AF(x)		((x) + 1)
#define STM32_PIN_ANALOG	(STM32_PIN_AF(15) + 1)
#define STM32_PIN_RSVD		(STM32_PIN_ANALOG + 1)
#define STM32_CONFIG_NUM	(STM32_PIN_RSVD + 1)

/*
 * package information
 * For DT backward compatibility, some value below is tagged as not to be changed
 * New packages can assume any other value
 */
enum stm32_pkg {
	STM32_PKG_AA = 0,  /* do not change */
	STM32_PKG_AB = 1,  /* do not change */
	STM32_PKG_AC = 2,  /* do not change */
	STM32_PKG_AD = 3,  /* do not change */
	STM32_PKG_AI = 8,  /* do not change */
	STM32_PKG_AJ,
	STM32_PKG_AK = 10, /* do not change */
	STM32_PKG_AL = 11, /* do not change */
	STM32_PKG_AM,
	STM32_PKG_AN,
	STM32_PKG_AO,

	/* keep this as last item */
	STM32_PKG_MAX,
};

struct stm32_desc_function {
	const char *name;
	const unsigned char num;
};

struct stm32_desc_pin {
	struct pinctrl_pin_desc pin;
	const struct stm32_desc_function functions[STM32_CONFIG_NUM];
	const unsigned int pkg;
};

#define STM32_PIN(_pin, ...)					\
	{							\
		.pin = _pin,					\
		.functions = {	\
			__VA_ARGS__},			\
	}

#define STM32_PIN_PKG(_pin, _pkg, ...)					\
	{							\
		.pin = _pin,					\
		.pkg  = _pkg,				\
		.functions = {	\
			__VA_ARGS__},			\
	}
#define STM32_FUNCTION(_num, _name)		\
	[_num] = {						\
		.num = _num,					\
		.name = _name,					\
	}

struct stm32_pinctrl_match_data {
	const struct stm32_desc_pin *pins;
	const unsigned int npins;
	bool secure_control;
	bool io_sync_control;
	bool rif_control;
};

/**
 * stm32_pctl_probe() - Common probe for stm32 pinctrl drivers.
 * @pdev: Pinctrl platform device.
 */
int stm32_pctl_probe(struct platform_device *pdev);

/**
 * stm32_pinctrl_suspend() - Common suspend for stm32 pinctrl drivers.
 * @dev: Pinctrl device.
 */
int stm32_pinctrl_suspend(struct device *dev);

/**
 * stm32_pinctrl_resume() - Common resume for stm32 pinctrl drivers.
 * @dev: Pinctrl device.
 */
int stm32_pinctrl_resume(struct device *dev);

#endif /* __PINCTRL_STM32_H */

