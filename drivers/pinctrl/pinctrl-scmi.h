/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2023 NXP
 */

#ifndef __DRIVERS_PINCTRL_SCMI_H
#define __DRIVERS_PINCTRL_SCMI_H

int pinctrl_scmi_imx_dt_node_to_map(struct pinctrl_dev *pctldev, struct device_node *np,
				    struct pinctrl_map **map, unsigned int *num_maps);

#endif /* __DRIVERS_PINCTRL_SCMI_H */
