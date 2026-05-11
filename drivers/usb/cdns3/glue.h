/* SPDX-License-Identifier: GPL-2.0 */
/*
 * glue.h - Cadence USB3 DRD glue header
 */

#ifndef __DRIVERS_USB_CDNS3_GLUE_H
#define __DRIVERS_USB_CDNS3_GLUE_H

#include <linux/types.h>

#include "core.h"

struct platform_device;

/**
 * struct cdns3_probe_data - Parameters passed to cdns3_core_probe()
 * @cdns: Cadence DRD controller context (allocated by the glue driver)
 * @pdev: Platform device for resources and IRQs
 */
struct cdns3_probe_data {
	struct cdns *cdns;
	struct platform_device *pdev;
};

/**
 * cdns3_core_probe - Initialize the Cadence USB3 platform core
 * @data: Controller context and platform device supplied by the glue layer
 *
 * Performs resource mapping, PHY setup, cdns_init(), role setup, and runtime PM
 * configuration for the standard platform binding of the Cadence USB3/USBSSP DRD IP.
 *
 * Return: 0 on success, negative errno on failure
 */
int cdns3_core_probe(const struct cdns3_probe_data *data);

/**
 * cdns3_core_remove - Tear down the Cadence USB3 platform core
 * @cdns: Controller context previously initialized by cdns3_core_probe()
 */
void cdns3_core_remove(struct cdns *cdns);

/*
 * The following callbacks are for glue drivers to invoke from their own
 * &dev_pm_ops, so platform-specific work can wrap the shared controller logic.
 */
int cdns3_runtime_suspend(struct cdns *cdns);
int cdns3_runtime_resume(struct cdns *cdns);
int cdns3_pm_suspend(struct cdns *cdns);
int cdns3_pm_resume(struct cdns *cdns);

#endif /* __DRIVERS_USB_CDNS3_GLUE_H */
