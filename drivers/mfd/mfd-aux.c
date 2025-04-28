// SPDX-License-Identifier: GPL-2.0-only
/*
 * MFD auxiliary device resources
 *
 * Copyright (c) 2025 Raag Jadav <raag.jadav@intel.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/device/devres.h>
#include <linux/export.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/mfd/aux.h>
#include <linux/types.h>

/**
 * maux_get_resource - get a resource for maux device
 * @maux: maux device
 * @type: resource type
 * @num: resource index
 *
 * Return: a pointer to the resource or NULL on failure.
 */
struct resource *maux_get_resource(struct maux_device *maux, unsigned int type, unsigned int num)
{
	u32 i;

	for (i = 0; i < maux->num_resources; i++) {
		struct resource *r = &maux->resource[i];

		if (type == resource_type(r) && num-- == 0)
			return r;
	}
	return NULL;
}
EXPORT_SYMBOL_NS_GPL(maux_get_resource, "MAUX_DEV");

#ifdef CONFIG_HAS_IOMEM
/**
 * devm_maux_get_and_ioremap_resource - get resource and call devm_ioremap_resource()
 *					for maux device
 *
 * @maux: maux device to use both for memory resource lookup as well as
 *        resource management
 * @index: resource index
 * @res: optional output parameter to store a pointer to the obtained resource.
 *
 * Return: a pointer to the remapped memory or an ERR_PTR() encoded error code
 * on failure.
 */
void __iomem *devm_maux_get_and_ioremap_resource(struct maux_device *maux, unsigned int index,
						 struct resource **res)
{
	struct resource *r;

	r = maux_get_resource(maux, IORESOURCE_MEM, index);
	if (res)
		*res = r;
	return devm_ioremap_resource(&maux->auxdev.dev, r);
}
EXPORT_SYMBOL_NS_GPL(devm_maux_get_and_ioremap_resource, "MAUX_DEV");

/**
 * devm_maux_ioremap_resource - call devm_ioremap_resource() for maux device
 *
 * @maux: maux device to use both for memory resource lookup as well as
 *        resource management
 * @index: resource index
 *
 * Return: a pointer to the remapped memory or an ERR_PTR() encoded error code
 * on failure.
 */
void __iomem *devm_maux_ioremap_resource(struct maux_device *maux, unsigned int index)
{
	return devm_maux_get_and_ioremap_resource(maux, index, NULL);
}
EXPORT_SYMBOL_NS_GPL(devm_maux_ioremap_resource, "MAUX_DEV");
#endif

/**
 * maux_get_irq_optional - get an optional IRQ for maux device
 * @maux: maux device
 * @num: IRQ number index
 *
 * Gets an IRQ for a maux device. Device drivers should check the return value
 * for errors so as to not pass a negative integer value to the request_irq()
 * APIs. This is the same as maux_get_irq(), except that it does not print an
 * error message if an IRQ can not be obtained.
 *
 * For example::
 *
 *		int irq = maux_get_irq_optional(maux, 0);
 *		if (irq < 0)
 *			return irq;
 *
 * Return: non-zero IRQ number on success, negative error number on failure.
 */
int maux_get_irq_optional(struct maux_device *maux, unsigned int num)
{
	struct resource *r;
	int ret = -ENXIO;

	r = maux_get_resource(maux, IORESOURCE_IRQ, num);
	if (!r)
		goto out;

	/*
	 * The resources may pass trigger flags to the irqs that need to be
	 * set up. It so happens that the trigger flags for IORESOURCE_BITS
	 * correspond 1-to-1 to the IRQF_TRIGGER* settings.
	 */
	if (r->flags & IORESOURCE_BITS) {
		struct irq_data *irqd;

		irqd = irq_get_irq_data(r->start);
		if (!irqd)
			goto out;
		irqd_set_trigger_type(irqd, r->flags & IORESOURCE_BITS);
	}

	ret = r->start;
	if (WARN(!ret, "0 is an invalid IRQ number\n"))
		ret = -EINVAL;
out:
	return ret;
}
EXPORT_SYMBOL_NS_GPL(maux_get_irq_optional, "MAUX_DEV");

/**
 * maux_get_irq - get an IRQ for maux device
 * @maux: maux device
 * @num: IRQ number index
 *
 * Gets an IRQ for a maux device and prints an error message if finding the IRQ
 * fails. Device drivers should check the return value for errors so as to not
 * pass a negative integer value to the request_irq() APIs.
 *
 * For example::
 *
 *		int irq = maux_get_irq(maux, 0);
 *		if (irq < 0)
 *			return irq;
 *
 * Return: non-zero IRQ number on success, negative error number on failure.
 */
int maux_get_irq(struct maux_device *maux, unsigned int num)
{
	int ret;

	ret = maux_get_irq_optional(maux, num);
	if (ret < 0)
		return dev_err_probe(&maux->auxdev.dev, ret, "IRQ index %u not found\n", num);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(maux_get_irq, "MAUX_DEV");
