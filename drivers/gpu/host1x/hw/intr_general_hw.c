// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * Tegra host1x General Interrupt Management
 */

#include <linux/interrupt.h>

#include "../actmon.h"
#include "../dev.h"

static irqreturn_t host1x_general_isr(int irq, void *dev_id)
{
	struct host1x *host = dev_id;
	unsigned long status;

	status = host1x_common_readl(host, HOST1X_COMMON_THOST_INTRSTATUS);

	if (status & HOST1X_COMMON_THOST_INTRSTATUS_VIC_ACTMON_INTR)
		host1x_actmon_handle_interrupt(host, HOST1X_CLASS_VIC);

	if (status & HOST1X_COMMON_THOST_INTRSTATUS_NVDEC_ACTMON_INTR)
		host1x_actmon_handle_interrupt(host, HOST1X_CLASS_NVDEC);

	host1x_common_writel(host, status, HOST1X_COMMON_THOST_INTRSTATUS);

	return IRQ_HANDLED;
}

static int host1x_intr_init_host_general(struct host1x *host)
{
	int err;

	if (host->general_irq <= 0 && !host->common_regs)
		return 0;

	host1x_hw_intr_disable_general_intrs(host);

	err = devm_request_threaded_irq(host->dev,
					host->general_irq,
					NULL, host1x_general_isr,
					IRQF_ONESHOT, "host1x_general",
					host);
	if (err < 0) {
		devm_free_irq(host->dev, host->general_irq, host);
		return err;
	}

	return 0;
}

static void host1x_intr_enable_general_intrs(struct host1x *host)
{
	if (host->general_irq <= 0 || !host->common_regs)
		return;

	/* Assign CCPLEX for host1x general interrupts */
	host1x_common_writel(host, 0x1, HOST1X_COMMON_INTR_CPU0_MASK);

	/* Allow host1x general interrupts go to CCPLEX only */
	host1x_common_writel(host, 0x1, HOST1X_COMMON_THOST_GLOBAL_INTRMASK);

	/* Enable host1x general interrupts */
	host1x_common_writel(host,
			     HOST1X_COMMON_THOST_INTRMASK_VIC_ACTMON(1) |
			     HOST1X_COMMON_THOST_INTRMASK_NVDEC_ACTMON(1),
			     HOST1X_COMMON_THOST_INTRMASK);
}

static void host1x_intr_disable_general_intrs(struct host1x *host)
{
	if (host->general_irq <= 0 || !host->common_regs)
		return;

	host1x_common_writel(host, 0x0, HOST1X_COMMON_THOST_INTRMASK);
}

static const struct host1x_intr_general_ops host1x_intr_general_ops = {
	.init_host_general = host1x_intr_init_host_general,
	.enable_general_intrs = host1x_intr_enable_general_intrs,
	.disable_general_intrs = host1x_intr_disable_general_intrs,
};
