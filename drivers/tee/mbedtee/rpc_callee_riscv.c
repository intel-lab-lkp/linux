// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Xing Loong <xing.xl.loong@gmail.com>
 * RISC-V MSI transport for the TEE->REE RPC callee.
 *
 * On RISC-V the TEE wakes the REE by writing a Message Signalled Interrupt
 * into the target hart's IMSIC (Incoming MSI Controller).  This file allocates
 * one platform MSI from the DT-provided MSI parent and registers the hard IRQ
 * handler supplied by the core callee layer.
 *
 * CPU hotplug: the common mbedtee_cpu_offline() callback in rpc_callee.c
 * detects when the callee CPU goes offline and calls riscv_update_affinity()
 * to migrate the MSI to another online CPU.
 */
#include <linux/interrupt.h>
#include <linux/msi.h>
#include <linux/of_irq.h>
#include <linux/smp.h>

#include "mbedtee_drv.h"

static int mbedtee_ring_set_hartid(struct mbedtee_device *mbedtee,
				   struct rpc_ringbuf *ring,
				   unsigned int cpu)
{
	long hartid = cpuid_to_hartid_map(cpu);

	if (hartid < 0) {
		dev_err(mbedtee->dev, "CPU%u has no valid hart-id\n", cpu);
		return -ERANGE;
	}

	WRITE_ONCE(ring->callee_hartid, (u32)hartid);
	return 0;
}

static void mbedtee_rpc_write_msi_msg(struct msi_desc *desc_msi,
				      struct msi_msg *msg)
{
	struct mbedtee_device *mbedtee = dev_get_drvdata(desc_msi->dev);

	if (mbedtee)
		mbedtee->transport.rpc_msi_msg = *msg;
}

static int riscv_transport_init(struct mbedtee_device *mbedtee,
				struct rpc_ringbuf *ring, irq_handler_t handler)
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;
	const struct cpumask *eff;
	struct mbedtee_rpc_transport_ctx *tctx = &mbedtee->transport;
	unsigned int cpu;
	struct device *dev = mbedtee->dev;
	int virq;
	int ret;

	/*
	 * Set a bootstrap callee_hartid (hart 0) so the TEE can poll
	 * even if MSI setup fails below.
	 */
	ret = mbedtee_ring_set_hartid(mbedtee, ring, 0);
	if (ret)
		return ret;

	dev_set_drvdata(dev, mbedtee);

	/*
	 * The MSI domain of an OF device is resolved only once, when the
	 * platform device is created (of_msi_configure() in of/platform.c).
	 * mbedtee's MSI parent is the IMSIC, whose MSI domain is created
	 * later, when the IMSIC builtin_platform_driver probes -- typically
	 * after this device has already been created with a NULL msi.domain.
	 *
	 * Deferred probing alone cannot recover from this: the driver core
	 * re-runs probe() but never re-runs of_msi_configure(), so the stale
	 * NULL domain would persist across every retry.  Re-resolve it here
	 * and defer until the IMSIC driver is up.  This mirrors the RISC-V
	 * APLIC/IOMMU platform-MSI drivers.
	 */
	if (!dev_get_msi_domain(dev)) {
		of_msi_configure(dev, dev->of_node);

		if (!dev_get_msi_domain(dev))
			return -EPROBE_DEFER;
	}

	ret = platform_device_msi_init_and_alloc_irqs(dev, 1,
						      mbedtee_rpc_write_msi_msg);
	if (ret) {
		dev_err(mbedtee->dev, "MSI alloc failed: %d\n", ret);
		return ret;
	}

	virq = msi_get_virq(dev, 0);
	if (virq <= 0) {
		dev_err(mbedtee->dev, "no MSI virq\n");
		ret = -ENOENT;
		goto err_msi;
	}

	ret = request_irq(virq, handler, 0, "mbedtee-rpc", mbedtee);
	if (ret) {
		dev_err(mbedtee->dev, "request_irq %d failed: %d\n", virq, ret);
		goto err_msi;
	}

	eff = irq_get_effective_affinity_mask(virq);
	cpu = eff ? cpumask_first(eff) : 0;

	dev_dbg(mbedtee->dev, "MSI addr 0x%x%08x data %d virq %d hart %ld\n",
		tctx->rpc_msi_msg.address_hi, tctx->rpc_msi_msg.address_lo,
		tctx->rpc_msi_msg.data, virq, cpuid_to_hartid_map(cpu));

	/* Inform TEE of the IMSIC identity used for T2R notifications. */
	ret = mbedtee_ring_set_hartid(mbedtee, ring, cpu);
	if (ret)
		goto err_irq;
	/* Ensure callee_hartid is visible before advertising MSI interrupt ID. */
	smp_store_release(&ring->callee_imsic_id, tctx->rpc_msi_msg.data);

	/* Track the active callee CPU for hotplug migration. */
	ctx->callee_virq = virq;
	cpumask_set_cpu(cpu, &ctx->callee_cpus);

	return 0;

err_irq:
	free_irq(virq, mbedtee);

err_msi:
	platform_device_msi_free_irqs_all(dev);
	return ret;
}

static void riscv_transport_uninit(struct mbedtee_device *mbedtee)
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;
	int virq;

	virq = ctx->callee_virq;
	ctx->callee_virq = 0;
	if (virq > 0)
		free_irq(virq, mbedtee);

	platform_device_msi_free_irqs_all(mbedtee->dev);
}

/*
 * Migrate the IMSIC MSI delivery to @new_cpu.
 *
 * Ordering guarantee for the TEE reader:
 *   1. Clear callee_imsic_id to 0 -- TEE skips IMSIC path while we migrate.
 *   2. Call irq_set_affinity() -- triggers mbedtee_rpc_write_msi_msg() which
 *      updates tctx->rpc_msi_msg with the new IMSIC identity.
 *   3. Update callee_hartid (WRITE_ONCE inside mbedtee_ring_set_hartid());
 *      no explicit barrier needed here since the release store on
 *      callee_imsic_id in step 4 provides the ordering.
 *   4. Publish new callee_imsic_id with smp_store_release -- TEE's
 *      smp_load_acquire on callee_imsic_id pairs with this store.
 */
static int riscv_update_affinity(struct mbedtee_device *mbedtee,
				 unsigned int new_cpu)
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;
	struct mbedtee_rpc_transport_ctx *tctx = &mbedtee->transport;
	struct rpc_ringbuf *ring = ctx->t2r_ring;
	int virq = ctx->callee_virq;

	if (!virq)
		return -ENODEV;

	/* Step 1: prevent TEE from using the stale IMSIC identity. */
	smp_store_release(&ring->callee_imsic_id, 0);

	/* Step 2: retarget the MSI; write_msi_msg callback updates rpc_msi_msg. */
	if (irq_set_affinity(virq, cpumask_of(new_cpu))) {
		dev_warn(mbedtee->dev, "irq_set_affinity to CPU%u failed\n",
			 new_cpu);
		return -EIO;
	}

	/* Step 3: update callee_hartid. */
	if (mbedtee_ring_set_hartid(mbedtee, ring, new_cpu))
		return -ERANGE;

	/* Step 4: publish new IMSIC id with release barrier. */
	smp_store_release(&ring->callee_imsic_id, tctx->rpc_msi_msg.data);

	dev_dbg(mbedtee->dev, "T2R callee migrated to CPU%u (hart %ld)\n",
		new_cpu, cpuid_to_hartid_map(new_cpu));
	return 0;
}

static const struct rpc_transport_ops riscv_transport_ops = {
	.init            = riscv_transport_init,
	.uninit          = riscv_transport_uninit,
	.update_affinity = riscv_update_affinity,
};

const struct rpc_transport_ops *mbedtee_get_rpc_transport_ops(void)
{
	return &riscv_transport_ops;
}
