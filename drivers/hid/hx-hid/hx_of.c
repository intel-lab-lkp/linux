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

#define UNUSED(x) ((void)(x))
static const char default_fw_name[] = BOOT_UPGRADE_FWNAME;

int himax_parse_dt(struct device_node *dt, struct himax_platform_data *pdata)
{
	/* pid_fw_name size = length of default_fw_name + length of "_XXXX" +
	 * length of ".bin" + null terminator.
	 */
	static char pid_fw_name[ARRAY_SIZE(default_fw_name) + 5 + 4 + 1] = {0};
	int tmp = 0;
	const int pid_prop_args = 2;
	u32 data = 0;
	int id_gpios[8] = {0};
	int counter = 0;
	int i = 0;
	s16 id_value = -1;
	int dts_irq = 0;
	int err = 0;
	struct device_node *panel_node = NULL;

	UNUSED(default_fw_name);
	if (!dt || !pdata) {
		E("DT: dev or pdata is NULL");
		return -EINVAL;
	}

	dts_irq = of_irq_get(dt, 0);
	D("DT: dts_irq = %d", dts_irq);
	if (dts_irq <= 0) {
		if (dts_irq == -EPROBE_DEFER)
			E("DT: dts_irq = -EPROBE_DEFER");
		return -EIO;
	}

	pdata->of_irq = dts_irq;
	pdata->gpio_irq = -1;

	pdata->gpio_reset = of_get_named_gpio(dt, "reset", 0);
	if (!gpio_is_valid(pdata->gpio_reset)) {
		I(" DT:gpio-rst value is not valid");
		return -EIO;
	}

	I(" DT:interrupt=%d, reset=%d",
	  pdata->of_irq, pdata->gpio_reset);

	counter = of_gpio_named_count(dt, "himax,id-gpios");
	if (counter > 0) {
		for (i = 0 ; i < counter ; i++) {
			id_gpios[i] = of_get_named_gpio(dt, "himax,id-gpios", i);
			if (!gpio_is_valid(id_gpios[i])) {
				I(" DT:gpio-id value is not valid");
				return -EIO;
			}
			I(" DT:gpio-id[%d]=%d", i, id_gpios[i]);
		}
		id_value = 0;
		for (i = 0 ; i < counter ; i++) {
			gpio_direction_input(id_gpios[i]);
			id_value |= gpio_get_value(id_gpios[i]) << i;
		}
		I(" DT:gpio-id value=%04X", id_value);
		pdata->panel_id = id_value;
	} else {
		pdata->panel_id = -1;
		D(" DT:gpio-id not found");
	}

	// himax,ic_det_delay unit is millisecond
	if (of_property_read_u32(dt, "himax,ic-det-delay-ms", &data)) {
		pdata->ic_det_delay = 0;
		D(" DT:himax,ic-det-delay-ms not found");
	} else {
		pdata->ic_det_delay = data;
		I(" DT:himax,ic-det-delay-ms=%d", pdata->ic_det_delay);
	}

	// himax,ic_resume_delay unit is millisecond
	if (of_property_read_u32(dt, "himax,ic-resume-delay-ms", &data)) {
		pdata->ic_resume_delay = 0;
		D(" DT:himax,ic-resume-delay-ms not found");
	} else {
		pdata->ic_resume_delay = data;
		I(" DT:himax,ic-resume-delay-ms=%d", pdata->ic_resume_delay);
	}

	if (of_property_read_bool(dt, "himax,has-flash")) {
		pdata->is_zf = false;
		D(" DT:himax,has-flash");
	} else {
		pdata->is_zf = true;
		I(" DT:himax,has-flash not found, load firmware from file");
	}

	if (of_property_read_bool(dt, "vccd-supply")) {
		pdata->vccd_supply = regulator_get(pdata->ts->dev, "vccd");
		if (IS_ERR(pdata->vccd_supply)) {
			E(" DT:failed to get vccd supply");
			err = PTR_ERR(pdata->vccd_supply);
			pdata->vccd_supply = NULL;
			return err;
		}
		I(" DT:vccd-supply=%p", pdata->vccd_supply);
	} else {
		pdata->vccd_supply = NULL;
	}

	if (of_property_read_bool(dt, "vcca-supply")) {
		pdata->vcca_supply = regulator_get(pdata->ts->dev, "vcca");
		if (IS_ERR(pdata->vcca_supply)) {
			E(" DT:failed to get vcca supply");
			err = PTR_ERR(pdata->vcca_supply);
			pdata->vcca_supply = NULL;
			return err;
		}
		I(" DT:vcca-supply=%p", pdata->vcca_supply);
	} else {
		pdata->vcca_supply = NULL;
	}

	/*
	 * check himax,pid first, if exist then check if it is single.
	 * Single case: himax,pid = <0x1002>; // 0x1002 is pid value
	 * Multiple case: himax,pid = <id_value0 00x1001>, <id_value1 0x1002>;
	 * When id_value >= 0, check the mapping listed to use the pid value.
	 */
	panel_node = of_get_child_by_name(dt, "panel");
	if (panel_node) {
		if (of_get_property(panel_node, "himax,pid", &data)) {
			counter = data / (sizeof(u32) * pid_prop_args);

			if (!counter) {
				// default case, no id->pid mappings
				if (of_property_read_u32(panel_node, "himax,pid", &data)) {
					pdata->pid = 0;
					D(" DT:himax,pid not found");
					goto GET_PID_END;
				} else {
					goto GET_PID_VALUE;
				}
			}

			if (id_value < 0) {
				E(" DT:himax,pid has no matched for id_value=%04X", id_value);
				pdata->pid = 0;
				goto GET_PID_END;
			}

			for (i = 0; i < counter; i++) {
				if (of_property_read_u32_index(panel_node, "himax,pid",
							       i * pid_prop_args, &tmp)) {
					D(" DT:himax,pid parsing error!");
					pdata->pid = 0;
					goto GET_PID_END;
				}

				if (of_property_read_u32_index(panel_node, "himax,pid",
							       i * pid_prop_args + 1, &data)) {
					D(" DT:himax,pid parsing error!");
					pdata->pid = 0;
					goto GET_PID_END;
				}

				if (tmp == id_value) {
					I(" DT:himax,pid mapping: id=%04X => pid=%04X, matched!",
					  tmp, data);
					i = counter;
				} else {
					I(" DT:himax,pid mapping: id=%04X => pid=%04X", tmp, data);
				}
			}

			if (counter == i) {
				E(" DT:himax,pid has no matched for id_value=%04X", id_value);
				pdata->pid = 0;
				goto GET_PID_END;
			}

GET_PID_VALUE:
			g_fw_boot_upgrade_name = pid_fw_name;
			pdata->pid = data;
			snprintf(pid_fw_name, sizeof(pid_fw_name), "%s_%04X%s",
				 BOOT_UPGRADE_FWNAME, pdata->pid, ".bin");
			I(" DT:himax,pid=%04X, fw_name=%s",
			  pdata->pid, pid_fw_name);
		} else {
			pdata->pid = 0;
			D(" DT:himax,pid not found");
		}
	} else {
		pdata->pid = 0;
		D(" DT:panel node not found");
	}
GET_PID_END:

	return 0;
}
