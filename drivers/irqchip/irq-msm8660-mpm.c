// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8x60 family (MSM8260/MSM8660/APQ8060) MPM (MSM Power Manager) wakeup interrupt controller
 *
 * The MPM is an always-on hardware block that keeps a small set of wake
 * sources alive while the application processor is powered down for
 * cpuidle Power Collapse or suspend-to-RAM. On MSM8x60 the
 * vMPM (virtual MPM) registers live INSIDE the RPM's 4 KB control block
 * at:
 *
 *   request (control) regs: RPM_BASE + 0x9d8  (ENABLE, DETECT_CTL,
 *                                              POLARITY, CLEAR)
 *   status (pending) regs:  RPM_BASE + 0xdf8  (== 0x9d8 + 0x420)
 *
 * The mainline qcom-mpm driver (drivers/irqchip/irq-qcom-mpm.c) is
 * fundamentally incompatible with this layout:
 *   - it assumes a dedicated MPM SRAM region separate from RPM;
 *   - it assumes a mailbox controller (IPCC) for wake notification;
 *   - it uses IRQCHIP_DECLARE which runs before platform devices exist,
 *     so of_find_device_by_node() returns NULL and the init silently
 *     hangs.
 *
 * This driver replicates the 2.6.35-palm `arch/arm/mach-msm/mpm.c`
 * mechanism as a regular platform driver: probes after platform
 * infrastructure is ready, ioremaps the vMPM sub-region of the RPM
 * control block (the qcom,rpm driver maps the surrounding area for
 * its own use; the two mappings overlap and neither claims exclusive
 * ownership), and uses the qcom-apcs-ipc mailbox for wake notification
 * (writing to GCC + 0x008 bit 1).
 *
 * Register access is done with readl_relaxed/writel_relaxed rather than
 * via the RPM syscon regmap. The IRQ core invokes our mask/unmask/
 * set_type/set_wake callbacks with the irq_desc's raw_spinlock_t held,
 * and syscon regmaps use a sleepable spinlock_t which on PREEMPT_RT
 * would deadlock under that raw lock. Direct MMIO is also what every
 * other SoC irqchip (qcom-pdc, gic-v3, ...) does.
 *
 * Two consumer interfaces:
 *
 *   1. Hierarchical irqdomain: for MPM pins that map to GIC SPIs (USB,
 *      HDMI, ...). Consumers wire their interrupts through this
 *      controller via interrupts-extended and the kernel manages
 *      enable / mask / set_type / set_wake via the IRQ subsystem.
 *
 *   2. Raw-pin API: for MPM pins that do NOT correspond to a GIC IRQ
 *      (SDC3_DAT1=21, SDC3_DAT3=22, SDC4_DAT1=23, SDC4_DAT3=24).
 *      These are physical wake-signal lines monitored by MPM
 *      directly. Consumers (mmci for SDC4 wake) call
 *      msm8660_mpm_set_pin_wake() etc. The consumer API establishes
 *      a device_link from consumer to producer so the MPM device
 *      cannot disappear while a consumer holds a handle.
 *
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 * Copyright (c) 2010-2012, The Linux Foundation (legacy mpm.c reference)
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <soc/qcom/msm8660-mpm.h>

/*
 * vMPM register offsets (relative to the start of the ioremap'd window
 * = RPM base + 0x9d8). Each register is two 32-bit slots because MPM
 * exposes 64 wake pins.
 */
#define MSM8660_MPM_REG_ENABLE		0x00
#define MSM8660_MPM_REG_DETECT_CTL	0x08
#define MSM8660_MPM_REG_POLARITY	0x10
#define MSM8660_MPM_REG_CLEAR		0x18

/* Status registers at +0x420 from vMPM base (== RPM + 0xdf8). */
#define MSM8660_MPM_STATUS_OFFSET	0x420

#define MSM8660_MPM_PIN_COUNT		64
#define MSM8660_MPM_REG_WIDTH		2

struct msm8660_mpm_pin {
	int pin;
	int hwirq;
};

struct msm8660_mpm {
	struct device *dev;
	void __iomem *base;
	struct irq_domain *domain;
	struct msm8660_mpm_pin *pin_map;
	unsigned int pin_map_count;
	int parent_irq;
	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;
};

/*
 * Singleton - there is only one MPM instance per SoC. msm8660_mpm_get()
 * returns this. Updates are serialised through the binding lifecycle so
 * a plain pointer is sufficient.
 */
static struct msm8660_mpm *msm8660_mpm_global;

static u32 msm8660_mpm_read(struct msm8660_mpm *mpm, unsigned int reg)
{
	return readl_relaxed(mpm->base + reg);
}

static void msm8660_mpm_write(struct msm8660_mpm *mpm, unsigned int reg,
			      u32 val)
{
	writel_relaxed(val, mpm->base + reg);
}

/*
 * Doorbell the RPM after touching the vMPM request registers. Without
 * this the RPM keeps using its last cached copy of the enable/detect/
 * polarity state and our configuration changes have no effect.
 *
 * Called from raw_spinlock_t-held contexts (irq_chip mask/unmask/
 * set_type/set_wake), so the mailbox driver must accept that. The
 * qcom-apcs-ipc mailbox just does a writel into the IPC trigger
 * register; it is safe under a raw lock.
 */
static void msm8660_mpm_doorbell(struct msm8660_mpm *mpm)
{
	int ret;

	if (!mpm->mbox_chan)
		return;

	ret = mbox_send_message(mpm->mbox_chan, NULL);
	if (ret < 0)
		dev_warn_ratelimited(mpm->dev,
				     "RPM doorbell failed: %d\n", ret);
}

static int msm8660_mpm_pin_to_hwirq(struct msm8660_mpm *mpm, int pin)
{
	int i;

	for (i = 0; i < mpm->pin_map_count; i++) {
		if (mpm->pin_map[i].pin == pin)
			return mpm->pin_map[i].hwirq;
	}
	return -ENOENT;
}

/*
 * IPC handler: MPM fires this IRQ when one or more enabled wake pins
 * have pending activity. Read pending status, CLEAR the pending bits
 * BEFORE dispatching the per-pin handlers so a fresh edge that arrives
 * during dispatch cannot be wiped out by a later CLEAR write, then
 * replay each pending pin through the irqdomain.
 */
static irqreturn_t msm8660_mpm_irq(int irq, void *data)
{
	struct msm8660_mpm *mpm = data;
	unsigned long pending[MSM8660_MPM_REG_WIDTH];
	unsigned long enable[MSM8660_MPM_REG_WIDTH];
	int i, j;

	for (i = 0; i < MSM8660_MPM_REG_WIDTH; i++) {
		pending[i] = msm8660_mpm_read(mpm,
			MSM8660_MPM_STATUS_OFFSET + i * 4);
		enable[i] = msm8660_mpm_read(mpm,
			MSM8660_MPM_REG_ENABLE + i * 4);
		pending[i] &= enable[i];

		/*
		 * Clear before dispatching: a new edge latched on this pin
		 * after this point will set the pending bit again and we
		 * will service it on the next IPC IRQ. Clearing AFTER the
		 * handler would race with that new latch and silently lose
		 * the new edge.
		 */
		if (pending[i])
			msm8660_mpm_write(mpm,
				MSM8660_MPM_REG_CLEAR + i * 4, pending[i]);
	}

	for (i = 0; i < MSM8660_MPM_REG_WIDTH; i++) {
		unsigned long bits = pending[i];

		for_each_set_bit(j, &bits, 32) {
			int pin = i * 32 + j;
			int hwirq = msm8660_mpm_pin_to_hwirq(mpm, pin);

			if (hwirq >= 0) {
				dev_dbg(mpm->dev, "wake pin %d -> hwirq %d\n",
					pin, hwirq);
				generic_handle_domain_irq(mpm->domain, hwirq);
			}
		}
	}

	return IRQ_HANDLED;
}

/*
 * Resolve an irq_data hwirq back to its MPM pin number, or -1 if the
 * pin is not in the qcom,mpm-pin-map (which would mean a consumer
 * bound to a hwirq that has no MPM mapping).
 */
static int msm8660_mpm_hwirq_to_pin(struct msm8660_mpm *mpm, unsigned int hwirq)
{
	int i;

	for (i = 0; i < mpm->pin_map_count; i++) {
		if (mpm->pin_map[i].hwirq == hwirq)
			return mpm->pin_map[i].pin;
	}
	return -1;
}

static void msm8660_mpm_enable_hwirq(struct irq_data *d, bool enable)
{
	struct msm8660_mpm *mpm = irq_data_get_irq_chip_data(d);
	int pin;
	u32 val, mask;

	pin = msm8660_mpm_hwirq_to_pin(mpm, d->hwirq);
	if (pin < 0)
		return;

	mask = BIT(pin % 32);
	val = msm8660_mpm_read(mpm,
		MSM8660_MPM_REG_ENABLE + (pin / 32) * 4);
	if (enable)
		val |= mask;
	else
		val &= ~mask;
	msm8660_mpm_write(mpm,
		MSM8660_MPM_REG_ENABLE + (pin / 32) * 4, val);

	msm8660_mpm_doorbell(mpm);
}

static void msm8660_mpm_mask_irq(struct irq_data *d)
{
	msm8660_mpm_enable_hwirq(d, false);
	irq_chip_mask_parent(d);
}

static void msm8660_mpm_unmask_irq(struct irq_data *d)
{
	msm8660_mpm_enable_hwirq(d, true);
	irq_chip_unmask_parent(d);
}

static int msm8660_mpm_set_type(struct irq_data *d, unsigned int type)
{
	struct msm8660_mpm *mpm = irq_data_get_irq_chip_data(d);
	u32 detect, polarity, mask;
	int pin;

	pin = msm8660_mpm_hwirq_to_pin(mpm, d->hwirq);
	if (pin < 0)
		return -ENOENT;

	mask = BIT(pin % 32);
	detect = msm8660_mpm_read(mpm,
		MSM8660_MPM_REG_DETECT_CTL + (pin / 32) * 4);
	polarity = msm8660_mpm_read(mpm,
		MSM8660_MPM_REG_POLARITY + (pin / 32) * 4);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		detect |= mask;
		polarity |= mask;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		detect |= mask;
		polarity &= ~mask;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		detect &= ~mask;
		polarity |= mask;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		detect &= ~mask;
		polarity &= ~mask;
		break;
	default:
		return -EINVAL;
	}

	msm8660_mpm_write(mpm,
		MSM8660_MPM_REG_DETECT_CTL + (pin / 32) * 4, detect);
	msm8660_mpm_write(mpm,
		MSM8660_MPM_REG_POLARITY + (pin / 32) * 4, polarity);

	msm8660_mpm_doorbell(mpm);

	return irq_chip_set_type_parent(d, type);
}

/*
 * Toggle MPM monitoring of the pin and propagate the wake request to
 * the parent GIC so it also stays alive during power-collapse.
 *
 * Without this callback the generic IRQ core would either silently
 * succeed (with IRQCHIP_SKIP_SET_WAKE) or fail outright, and neither
 * the MPM nor the GIC would actually be programmed to wake the system.
 */
static int msm8660_mpm_set_wake(struct irq_data *d, unsigned int on)
{
	struct msm8660_mpm *mpm = irq_data_get_irq_chip_data(d);
	int pin;

	pin = msm8660_mpm_hwirq_to_pin(mpm, d->hwirq);
	if (pin < 0)
		return -ENOENT;

	msm8660_mpm_enable_hwirq(d, !!on);

	return irq_chip_set_wake_parent(d, on);
}

static struct irq_chip msm8660_mpm_chip = {
	.name			= "msm8660-mpm",
	.irq_mask		= msm8660_mpm_mask_irq,
	.irq_unmask		= msm8660_mpm_unmask_irq,
	.irq_set_type		= msm8660_mpm_set_type,
	.irq_set_wake		= msm8660_mpm_set_wake,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.flags			= IRQCHIP_MASK_ON_SUSPEND,
};

static int msm8660_mpm_domain_alloc(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs,
				    void *data)
{
	struct msm8660_mpm *mpm = domain->host_data;
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec parent_fwspec;
	irq_hw_number_t hwirq;
	int i, ret;

	if (fwspec->param_count != 2)
		return -EINVAL;

	hwirq = fwspec->param[0];

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &msm8660_mpm_chip, mpm);

	parent_fwspec.fwnode = domain->parent->fwnode;
	parent_fwspec.param_count = 3;
	parent_fwspec.param[0] = 0;
	parent_fwspec.param[1] = hwirq;
	parent_fwspec.param[2] = fwspec->param[1];

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					   &parent_fwspec);
	if (ret) {
		dev_err(mpm->dev, "irq_domain_alloc_irqs_parent failed: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static int msm8660_mpm_translate(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *hwirq, unsigned int *type)
{
	if (fwspec->param_count != 2)
		return -EINVAL;

	*hwirq = fwspec->param[0];
	*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static const struct irq_domain_ops msm8660_mpm_domain_ops = {
	.translate	= msm8660_mpm_translate,
	.alloc		= msm8660_mpm_domain_alloc,
	.free		= irq_domain_free_irqs_common,
};

/* ===================================================================
 * Raw-pin consumer API
 * ===================================================================
 */

/**
 * msm8660_mpm_get() - acquire a handle to the MPM for raw-pin use
 * @consumer: device of the consumer driver
 * @np: optional device-tree node containing a phandle reference
 * @propname: optional property name for that phandle (e.g. "qcom,mpm")
 *
 * Returns the singleton MPM handle, ERR_PTR(-EPROBE_DEFER) if the MPM
 * driver has not finished probing yet, or ERR_PTR(-ENOENT) if @np is
 * given and the phandle does not resolve.
 *
 * On success this also establishes a consumer-supplier device_link so
 * the MPM device cannot be unbound while the consumer holds the
 * handle. The link is auto-removed when @consumer is unbound.
 */
struct msm8660_mpm *msm8660_mpm_get(struct device *consumer,
				    struct device_node *np,
				    const char *propname)
{
	struct device_node *mpm_np;
	struct device_link *link;

	if (!msm8660_mpm_global)
		return ERR_PTR(-EPROBE_DEFER);

	if (np && propname) {
		mpm_np = of_parse_phandle(np, propname, 0);
		if (!mpm_np)
			return ERR_PTR(-ENOENT);
		of_node_put(mpm_np);
	}

	if (!consumer)
		return msm8660_mpm_global;

	link = device_link_add(consumer, msm8660_mpm_global->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		dev_warn(consumer, "failed to link to MPM, deferring\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	return msm8660_mpm_global;
}
EXPORT_SYMBOL_GPL(msm8660_mpm_get);

/**
 * msm8660_mpm_enable_pin() - enable/disable MPM monitoring of a pin
 * @mpm: handle from msm8660_mpm_get()
 * @pin: MPM pin index (0..MSM8660_MPM_PIN_COUNT-1)
 * @enable: true to monitor, false to ignore
 *
 * Programs the ENABLE register directly. Intended for "raw" wake pins
 * (SDC3_DAT1=21, SDC3_DAT3=22, SDC4_DAT1=23, SDC4_DAT3=24) that have no
 * GIC IRQ mapping. For pins that DO have a GIC mapping (in
 * qcom,mpm-pin-map), use the irqdomain path instead.
 */
int msm8660_mpm_enable_pin(struct msm8660_mpm *mpm, unsigned int pin,
			   bool enable)
{
	u32 val;

	if (!mpm || pin >= MSM8660_MPM_PIN_COUNT)
		return -EINVAL;

	val = msm8660_mpm_read(mpm,
		MSM8660_MPM_REG_ENABLE + (pin / 32) * 4);
	if (enable)
		val |= BIT(pin % 32);
	else
		val &= ~BIT(pin % 32);
	msm8660_mpm_write(mpm,
		MSM8660_MPM_REG_ENABLE + (pin / 32) * 4, val);

	msm8660_mpm_doorbell(mpm);

	return 0;
}
EXPORT_SYMBOL_GPL(msm8660_mpm_enable_pin);

/**
 * msm8660_mpm_set_pin_wake() - mark a pin as wake-capable
 * @mpm: handle
 * @pin: MPM pin index
 * @on:  true to allow this pin to wake the system, false to clear.
 *
 * Equivalent to msm8660_mpm_enable_pin() on MSM8660 - the hardware has
 * a single ENABLE register, not separate enable + wake masks. The name
 * is kept for API parity with the legacy mpm.h interface so consumers
 * can express the wake-source intent explicitly.
 */
int msm8660_mpm_set_pin_wake(struct msm8660_mpm *mpm, unsigned int pin,
			     bool on)
{
	return msm8660_mpm_enable_pin(mpm, pin, on);
}
EXPORT_SYMBOL_GPL(msm8660_mpm_set_pin_wake);

/**
 * msm8660_mpm_set_pin_type() - set trigger type for a raw MPM pin
 * @mpm: handle
 * @pin: MPM pin index
 * @flow_type: standard IRQ_TYPE_* constants
 *
 * On MSM8660 the trigger config is split across DETECT_CTL (edge vs
 * level) and POLARITY (rising/high vs falling/low).
 */
int msm8660_mpm_set_pin_type(struct msm8660_mpm *mpm, unsigned int pin,
			     unsigned int flow_type)
{
	u32 detect, polarity;
	bool edge, polarity_high;

	if (!mpm || pin >= MSM8660_MPM_PIN_COUNT)
		return -EINVAL;

	edge = !!(flow_type & IRQ_TYPE_EDGE_BOTH);
	polarity_high = !!(flow_type & (IRQ_TYPE_EDGE_RISING |
					IRQ_TYPE_LEVEL_HIGH));

	detect = msm8660_mpm_read(mpm,
		MSM8660_MPM_REG_DETECT_CTL + (pin / 32) * 4);
	polarity = msm8660_mpm_read(mpm,
		MSM8660_MPM_REG_POLARITY + (pin / 32) * 4);

	if (edge)
		detect |= BIT(pin % 32);
	else
		detect &= ~BIT(pin % 32);

	if (polarity_high)
		polarity |= BIT(pin % 32);
	else
		polarity &= ~BIT(pin % 32);

	msm8660_mpm_write(mpm,
		MSM8660_MPM_REG_DETECT_CTL + (pin / 32) * 4, detect);
	msm8660_mpm_write(mpm,
		MSM8660_MPM_REG_POLARITY + (pin / 32) * 4, polarity);

	msm8660_mpm_doorbell(mpm);

	return 0;
}
EXPORT_SYMBOL_GPL(msm8660_mpm_set_pin_type);

/* ===================================================================
 * Platform driver
 * ===================================================================
 */

static int msm8660_mpm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct msm8660_mpm *mpm;
	struct irq_domain *parent_domain;
	struct device_node *parent_np;
	struct resource *res;
	int ret, i;

	if (msm8660_mpm_global)
		return dev_err_probe(dev, -EBUSY,
				     "only one MPM instance is supported\n");

	mpm = devm_kzalloc(dev, sizeof(*mpm), GFP_KERNEL);
	if (!mpm)
		return -ENOMEM;

	mpm->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -ENODEV, "missing reg property\n");

	/*
	 * Use a non-exclusive mapping: the qcom,rpm driver maps the
	 * surrounding RPM control block via its own platform resource,
	 * and our vMPM sub-region overlaps that mapping. devm_ioremap()
	 * does not call request_mem_region() so there is no conflict.
	 */
	mpm->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!mpm->base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to ioremap vMPM at %pR\n", res);

	/*
	 * Parse pin map (IRQ-mapped wake pins; raw pins like SDC4_DAT1
	 * are not listed here - they are accessed via the pin-API).
	 */
	ret = of_property_count_u32_elems(np, "qcom,mpm-pin-map");
	if (ret < 0 || ret % 2)
		return dev_err_probe(dev, -EINVAL,
				     "invalid qcom,mpm-pin-map\n");

	mpm->pin_map_count = ret / 2;
	mpm->pin_map = devm_kcalloc(dev, mpm->pin_map_count,
				    sizeof(*mpm->pin_map), GFP_KERNEL);
	if (!mpm->pin_map)
		return -ENOMEM;

	for (i = 0; i < mpm->pin_map_count; i++) {
		u32 pin, hwirq;

		of_property_read_u32_index(np, "qcom,mpm-pin-map",
					   i * 2, &pin);
		of_property_read_u32_index(np, "qcom,mpm-pin-map",
					   i * 2 + 1, &hwirq);

		if (pin >= MSM8660_MPM_PIN_COUNT)
			return dev_err_probe(dev, -EINVAL,
				"qcom,mpm-pin-map entry %d: pin %u >= %u\n",
				i, pin, MSM8660_MPM_PIN_COUNT);

		mpm->pin_map[i].pin = pin;
		mpm->pin_map[i].hwirq = hwirq;

		dev_dbg(dev, "pin map: pin %u -> hwirq %u\n", pin, hwirq);
	}

	parent_np = of_irq_find_parent(np);
	if (!parent_np)
		return dev_err_probe(dev, -ENODEV,
				     "failed to find parent interrupt controller\n");

	parent_domain = irq_find_host(parent_np);
	of_node_put(parent_np);
	if (!parent_domain)
		return dev_err_probe(dev, -ENODEV,
				     "failed to find parent IRQ domain\n");

	mpm->domain = irq_domain_create_hierarchy(parent_domain, 0,
						  MSM8660_MPM_PIN_COUNT,
						  of_fwnode_handle(np),
						  &msm8660_mpm_domain_ops,
						  mpm);
	if (!mpm->domain)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to create IRQ domain\n");

	mpm->parent_irq = platform_get_irq(pdev, 0);
	if (mpm->parent_irq < 0) {
		ret = mpm->parent_irq;
		goto err_remove_domain;
	}

	/*
	 * Mailbox channel for poking MPM to re-read its config. Get this
	 * BEFORE registering our IRQ handler so that doorbell-from-IRQ
	 * never sees a partially-initialised channel pointer.
	 */
	mpm->mbox_client.dev = dev;
	mpm->mbox_client.knows_txdone = true;
	mpm->mbox_chan = mbox_request_channel(&mpm->mbox_client, 0);
	if (IS_ERR(mpm->mbox_chan)) {
		ret = PTR_ERR(mpm->mbox_chan);
		mpm->mbox_chan = NULL;
		if (ret == -EPROBE_DEFER)
			goto err_remove_domain;
		dev_warn(dev, "no mailbox channel: %d (continuing without RPM doorbell)\n",
			 ret);
	}

	/*
	 * Register the parent IRQ last and use plain request_irq() (not
	 * devm_*) so we can free it explicitly in ->remove() before
	 * irq_domain_remove(). With devm_request_irq() the handler
	 * outlives irq_domain_remove() and a wake event arriving in the
	 * removal window would dereference a freed domain pointer.
	 */
	ret = request_irq(mpm->parent_irq, msm8660_mpm_irq,
			  IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND,
			  "msm8660-mpm", mpm);
	if (ret) {
		dev_err(dev, "failed to request IRQ %d: %d\n",
			mpm->parent_irq, ret);
		goto err_free_mbox;
	}

	platform_set_drvdata(pdev, mpm);
	msm8660_mpm_global = mpm;

	dev_info(dev, "ready: %d pin mappings, irq=%d\n",
		 mpm->pin_map_count, mpm->parent_irq);

	return 0;

err_free_mbox:
	if (mpm->mbox_chan)
		mbox_free_channel(mpm->mbox_chan);
err_remove_domain:
	irq_domain_remove(mpm->domain);
	return ret;
}

static void msm8660_mpm_remove(struct platform_device *pdev)
{
	struct msm8660_mpm *mpm = platform_get_drvdata(pdev);

	/*
	 * Tear down in strict reverse order: drop the singleton so new
	 * consumers cannot grab a handle, free the IRQ so the handler
	 * cannot fire again, free the mailbox channel, then remove the
	 * domain. Consumer device_links established in msm8660_mpm_get()
	 * prevent the parent device from being unbound while a consumer
	 * still holds a handle.
	 */
	msm8660_mpm_global = NULL;
	free_irq(mpm->parent_irq, mpm);
	if (mpm->mbox_chan)
		mbox_free_channel(mpm->mbox_chan);
	irq_domain_remove(mpm->domain);
}

static const struct of_device_id msm8660_mpm_of_match[] = {
	{ .compatible = "qcom,msm8660-mpm" },
	{}
};
MODULE_DEVICE_TABLE(of, msm8660_mpm_of_match);

static struct platform_driver msm8660_mpm_driver = {
	.probe		= msm8660_mpm_probe,
	.remove		= msm8660_mpm_remove,
	.driver		= {
		.name	= "msm8660-mpm",
		.of_match_table = msm8660_mpm_of_match,
	},
};

static int __init msm8660_mpm_init(void)
{
	return platform_driver_register(&msm8660_mpm_driver);
}
subsys_initcall(msm8660_mpm_init);

static void __exit msm8660_mpm_exit(void)
{
	platform_driver_unregister(&msm8660_mpm_driver);
}
module_exit(msm8660_mpm_exit);

MODULE_DESCRIPTION("Qualcomm MSM8x60 MPM wakeup interrupt controller");
MODULE_LICENSE("GPL");
