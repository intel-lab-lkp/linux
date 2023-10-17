// SPDX-License-Identifier: GPL-2.0
/*  Himax Driver Code for Common IC to simulate HID
 *
 *  Copyright (C) 2023 Himax Corporation.
 *
 *  This software is licensed under the terms of the GNU General Public
 *  License version 2,  as published by the Free Software Foundation,  and
 *  may be copied,  distributed,  and modified under those terms.
 *
 *  This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "hx_core.h"

int himax_parse_acpi(struct device *dev,
		     struct himax_platform_data *pdata)
{
	int ret = 0;
	struct gpio_desc *desc;
	const u32 interrupt_pin_idx = 0;
	// const u32 reset_pin_idx = 1;
	const char *interrupt_pin_dsd_name = "irq"; // to name "irq-gpios"
	const char *reset_pin_dsd_name = "reset"; // to name "reset-gpios"

	D("Entered");
	if (!dev || !pdata) {
		E("ACPI: dev or pdata is NULL");
		ret = -EINVAL;
		goto END;
	}
	desc = gpiod_get(dev, interrupt_pin_dsd_name, GPIOD_IN);
	if (IS_ERR(desc)) {
		E("ACPI: gpiod_get(%s) failed : %ld", interrupt_pin_dsd_name,
		  PTR_ERR(desc));
		I("Try to get by index");
		desc = gpiod_get_index(dev, NULL, interrupt_pin_idx, GPIOD_IN);
		if (IS_ERR(desc)) {
			E("ACPI: gpiod_get_index(%d) failed : %ld", interrupt_pin_idx,
			  PTR_ERR(desc));
			ret = -EINVAL;
			goto END;
		} else if (desc_to_gpio(desc) == 0) {
			E("ACPI: gpio_irq value is not valid : %d",
			  desc_to_gpio(desc));
			ret = -EINVAL;
			goto END;
		} else {
			pdata->gpio_irq = desc_to_gpio(desc);
		}
	} else if (desc_to_gpio(desc) == 0) {
		E("ACPI: gpio_irq value is not valid : %d", desc_to_gpio(desc));
		ret = -EINVAL;
		goto END;
	} else {
		pdata->gpio_irq = desc_to_gpio(desc);
	}

	desc = gpiod_get(dev, reset_pin_dsd_name, GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		E("ACPI: gpiod_get(%s) failed : %ld", reset_pin_dsd_name,
		  PTR_ERR(desc));
		ret = -EINVAL;
		goto END;
	} else if (desc_to_gpio(desc) == 0) {
		E("ACPI: gpio-reset value is not valid : %d",
		  desc_to_gpio(desc));
		ret = -EINVAL;
		goto END;
	} else {
		pdata->gpio_reset = desc_to_gpio(desc);
	}

	I("[ACPI]irq gpio %d, reset gpio %d",
	  pdata->gpio_irq, pdata->gpio_reset);

END:
	return ret;
}
