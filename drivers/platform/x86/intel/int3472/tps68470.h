/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TI TPS68470 PMIC platform data definition.
 *
 * Copyright (c) 2021 Red Hat Inc.
 *
 * Red Hat authors:
 * Hans de Goede <hdegoede@redhat.com>
 */

#ifndef _INTEL_SKL_INT3472_TPS68470_H
#define _INTEL_SKL_INT3472_TPS68470_H

struct gpiod_lookup_table;
struct tps68470_clk_consumer;
struct tps68470_regulator_platform_data;

struct int3472_tps68470_board_data {
	const char *dev_name;
	const struct tps68470_regulator_platform_data *tps68470_regulator_pdata;
	/*
	 * Static clock consumers.  When n_clk_consumers is non-zero these
	 * are used in place of for_each_acpi_consumer_dev() to build the
	 * tps68470-clk platform data.  Needed on platforms where a sensor's
	 * ACPI _DEP does not list the INT3472 device, causing that sensor
	 * to be missed by the ACPI dependency traversal.
	 */
	unsigned int n_clk_consumers;
	const struct tps68470_clk_consumer *clk_consumers;
	unsigned int n_gpiod_lookups;
	struct gpiod_lookup_table *tps68470_gpio_lookup_tables[];
};

const struct int3472_tps68470_board_data *int3472_tps68470_get_board_data(const char *dev_name);

#endif
