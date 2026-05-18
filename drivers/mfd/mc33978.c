// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 David Jander <david@protonic.nl>, Protonic Holland
 * Copyright (C) 2026 Oleksij Rempel <kernel@pengutronix.de>, Pengutronix
 *
 * MC33978/MC34978 MFD Driver - Device binding and power sequencing only.
 * Core logic (regmap/IRQ/events) lives in separate mc33978-core module to
 * isolate complex SPI protocol from simple MFD device registration.
 */

#include <linux/irqdomain.h>
#include <linux/mfd/core.h>
#include <linux/mfd/mc33978.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

/*
 * MC33978 vs MC34978: Identical register map and pin configuration; only
 * temperature range differs (MC33978: -40°C to +125°C, MC34978: -40°C to
 * +150°C). Both variants share IRQ layout and resources. Device type
 * preserved for potential errata handling and hwmon-specific attributes.
 */
enum mc33978_device_type {
	MC33978 = 1,  /* Must be non-zero: used as match_data pointer value */
	MC34978 = 2,
};

static const struct resource mc33978_hwmon_resources[] = {
	DEFINE_RES_IRQ(MC33978_HWIRQ_FAULT),
};

static const struct mfd_cell mc33978_cells[] = {
	MFD_CELL_NAME("mc33978-pinctrl"),
	MFD_CELL_RES("mc33978-hwmon", mc33978_hwmon_resources),
	MFD_CELL_NAME("mc33978-mux"),
};

static const struct mfd_cell mc34978_cells[] = {
	MFD_CELL_NAME("mc34978-pinctrl"),
	MFD_CELL_RES("mc34978-hwmon", mc33978_hwmon_resources),
	MFD_CELL_NAME("mc34978-mux"),
};

struct mc33978_ddata {
	struct irq_domain *domain;
};

static int mc33978_mfd_probe(struct spi_device *spi)
{
	unsigned long type;
	const struct mfd_cell *cells;
	struct device *dev = &spi->dev;
	struct mc33978_ddata *ddata;
	int num_cells;
	int ret;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	spi_set_drvdata(spi, ddata);

	/* Power up chip: VDDQ first per datasheet sequencing requirements */
	ret = devm_regulator_get_enable(dev, "vddq");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable VDDQ supply\n");

	ret = devm_regulator_get_enable(dev, "vbatp");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable VBATP supply\n");

	/*
	 * Core module creates regmap/IRQ domain/event handling. Separated
	 * because custom SPI protocol needs complex regmap_bus implementation.
	 *
	 * API contract: mc33978_core_init() initializes domain_out to NULL on
	 * entry. On success (return 0), domain_out is guaranteed to point to a
	 * valid IRQ domain. On error, domain_out remains NULL. This allows
	 * safe dereference of ddata->domain below without NULL check after
	 * verifying ret == 0.
	 */
	ret = mc33978_core_init(dev, spi, &ddata->domain);
	if (ret)
		return ret;

	/*
	 * Validate IRQ domain size before passing to child devices.
	 * hwirq_max is inclusive, we need MC33978_NUM_IRQS entries (0..22).
	 */
	if (ddata->domain->hwirq_max < MC33978_NUM_IRQS - 1)
		return dev_err_probe(dev, -EINVAL,
				     "IRQ domain has insufficient hwirqs (need %d)\n",
				     MC33978_NUM_IRQS);

	/*
	 * Device type stored as pointer value in OF/SPI ID tables.
	 * Explicit zero-check: spi_get_device_match_data() won't return NULL
	 * since probe matched, but .data could be NULL if table misconfigured.
	 */
	type = (unsigned long)spi_get_device_match_data(spi);
	if (!type)
		return dev_err_probe(dev, -EINVAL, "missing device match data\n");

	switch (type) {
	case MC33978:
		cells = mc33978_cells;
		num_cells = ARRAY_SIZE(mc33978_cells);
		break;
	case MC34978:
		cells = mc34978_cells;
		num_cells = ARRAY_SIZE(mc34978_cells);
		break;
	default:
		return dev_err_probe(dev, -ENODEV, "unknown device type\n");
	}

	/*
	 * Child devices inherit IRQ domain for platform_get_irq(). devm cleanup
	 * order is critical (LIFO - Last In, First Out):
	 *
	 * Teardown sequence:
	 * 1. THIS: devm_mfd_add_devices() cleanup
	 *    - Child devices removed, child devm_request_irq() freed
	 * 2. Core: devm_request_threaded_irq() cleanup in mc33978_core_init()
	 *    - Parent IRQ handler freed, no new events triggered
	 * 3. Core: devm_add_action(mc33978_teardown) cleanup
	 *    - event_work canceled via cancel_work_sync()
	 * 4. Core: devm_regmap_init() cleanup
	 *    - Regmap destroyed (safe: work stopped, IRQ freed)
	 * 5. Core: devm_add_action(mc33978_irq_domain_remove) in mc33978_irq_init()
	 *    - IRQ domain removed (safe: children gone, work stopped)
	 *
	 * Step 3 is critical: event_work accesses both regmap and IRQ domain.
	 * cancel_work_sync() in mc33978_teardown() ensures the worker completes
	 * before steps 4-5 destroy the resources it uses.
	 *
	 * Core module manually calls irq_dispose_mapping() for all hwirqs before
	 * irq_domain_remove() because free_irq() doesn't dispose mappings, and
	 * irq_domain_remove() expects an empty radix tree.
	 */
	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				   cells, num_cells,
				   NULL, 0, ddata->domain);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add MFD child devices\n");

	return 0;
}

static const struct of_device_id mc33978_mfd_of_match[] = {
	{ .compatible = "nxp,mc33978", .data = (void *)MC33978 },
	{ .compatible = "nxp,mc34978", .data = (void *)MC34978 },
	{ }
};
MODULE_DEVICE_TABLE(of, mc33978_mfd_of_match);

static const struct spi_device_id mc33978_mfd_spi_id[] = {
	{ .name = "mc33978", .driver_data = MC33978 },
	{ .name = "mc34978", .driver_data = MC34978 },
	{ }
};
MODULE_DEVICE_TABLE(spi, mc33978_mfd_spi_id);

static struct spi_driver mc33978_mfd_driver = {
	.driver = {
		.name = "mc33978",
		.of_match_table = mc33978_mfd_of_match,
		/*
		 * Suppress bind/unbind via sysfs. The pinctrl child driver has
		 * suppress_bind_attrs=true due to a pinctrl subsystem bug, which
		 * means this MFD parent cannot be unbound either (device links
		 * require all children to unbind first).
		 */
		.suppress_bind_attrs = true,
	},
	.probe = mc33978_mfd_probe,
	.id_table = mc33978_mfd_spi_id,
};
module_spi_driver(mc33978_mfd_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("NXP MC33978/MC34978 MFD driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: mc33978-core");
