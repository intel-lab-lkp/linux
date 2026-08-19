// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 * ARM/ARM64 GIC transport for the TEE->REE RPC callee.
 *
 * On ARM/ARM64 the TEE raises a rising-edge software-generated interrupt
 * described by the "interrupts" property of the mbedtee DT node.  This file
 * obtains that virq via of_irq_get() and registers the hard IRQ handler
 * supplied by the core callee layer.
 */
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/of_irq.h>

#include "mbedtee_drv.h"

static int arm_transport_init(struct mbedtee_device *mbedtee,
			      struct rpc_ringbuf *ring, irq_handler_t handler)
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;
	struct mbedtee_rpc_transport_ctx *tctx = &mbedtee->transport;
	int ret;

	tctx->rpc_notify_virq = of_irq_get(mbedtee->dev->of_node, 0);
	if (tctx->rpc_notify_virq <= 0)
		return tctx->rpc_notify_virq ? tctx->rpc_notify_virq : -ENODEV;

	ret = request_irq(tctx->rpc_notify_virq, handler,
			  0, "mbedtee-rpc", mbedtee);
	if (ret) {
		dev_warn(mbedtee->dev, "request_irq %d failed: %d\n",
			 tctx->rpc_notify_virq, ret);
		tctx->rpc_notify_virq = 0;
		return ret;
	}

	/*
	 * Spread the T2R SPI across all online CPUs in Linux.
	 * The TEE needs no CPU information from Linux: it broadcasts
	 * via GIC hardware (GICv2 ITARGETS=0xFF / GICv3 IROUTER.IRM=1).
	 */
	cpumask_copy(&ctx->callee_cpus, cpu_online_mask);
	if (irq_set_affinity(tctx->rpc_notify_virq, &ctx->callee_cpus))
		dev_warn(mbedtee->dev, "irq_set_affinity failed\n");

	dev_dbg(mbedtee->dev, "t2r-irq %d\n", tctx->rpc_notify_virq);
	return 0;
}

static void arm_transport_uninit(struct mbedtee_device *mbedtee)
{
	struct mbedtee_rpc_transport_ctx *tctx = &mbedtee->transport;

	if (tctx->rpc_notify_virq > 0) {
		free_irq(tctx->rpc_notify_virq, mbedtee);
		tctx->rpc_notify_virq = 0;
	}
}

/*
 * ARM T2R uses a GIC SPI: the TEE broadcasts it via GIC hardware
 * (GICv2 ITARGETS=0xFF or GICv3 IROUTER.IRM=1) to any alive CPU.
 * TrustZone shares physical CPUs -- no ring update is ever needed.
 *
 * @new_cpu is ignored: the IRQ is re-affined to ctx->callee_cpus which
 * the core callee layer keeps in sync (dying CPU already removed before
 * this callback is invoked, so the mask is always accurate).
 */
static int arm_update_affinity(struct mbedtee_device *mbedtee,
			       unsigned int new_cpu)
{
	struct mbedtee_rpc_transport_ctx *tctx = &mbedtee->transport;

	if (tctx->rpc_notify_virq > 0 &&
	    irq_set_affinity(tctx->rpc_notify_virq, &mbedtee->t2r.callee_cpus))
		dev_warn(mbedtee->dev, "irq_set_affinity failed\n");

	return 0;
}

static const struct rpc_transport_ops arm_transport_ops = {
	.init            = arm_transport_init,
	.uninit          = arm_transport_uninit,
	.update_affinity = arm_update_affinity,
};

const struct rpc_transport_ops *mbedtee_get_rpc_transport_ops(void)
{
	return &arm_transport_ops;
}
