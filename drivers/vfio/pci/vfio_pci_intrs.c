// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI interrupt handling
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/eventfd.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/file.h>
#include <linux/vfio.h>
#include <linux/wait.h>
#include <linux/slab.h>

#include "vfio_pci_priv.h"

/*
 * Interrupt Message Store (IMS) private interrupt context data
 * @vdev:		Virtual device. Used for name of device in
 *			request_irq().
 * @pdev:		PCI device owning the IMS domain from where
 *			interrupts are allocated.
 * @default_cookie:	Default cookie used for IMS interrupts without unique
 *			cookie.
 */
struct vfio_pci_ims {
	struct vfio_device		*vdev;
	struct pci_dev			*pdev;
	union msi_instance_cookie	default_cookie;
};

struct vfio_pci_irq_ctx {
	bool			emulated:1;
	struct eventfd_ctx	*trigger;
	struct virqfd		*unmask;
	struct virqfd		*mask;
	char			*name;
	bool			masked;
	struct irq_bypass_producer	producer;
	int			virq;
	int			ims_id;
	union msi_instance_cookie	icookie;
};

static bool irq_is(struct vfio_pci_intr_ctx *intr_ctx, int type)
{
	return intr_ctx->irq_type == type;
}

static bool is_intx(struct vfio_pci_core_device *vdev)
{
	return vdev->intr_ctx.irq_type == VFIO_PCI_INTX_IRQ_INDEX;
}

static bool is_irq_none(struct vfio_pci_intr_ctx *intr_ctx)
{
	return !(intr_ctx->irq_type == VFIO_PCI_INTX_IRQ_INDEX ||
		 intr_ctx->irq_type == VFIO_PCI_MSI_IRQ_INDEX ||
		 intr_ctx->irq_type == VFIO_PCI_MSIX_IRQ_INDEX);
}

static
struct vfio_pci_irq_ctx *vfio_irq_ctx_get(struct vfio_pci_intr_ctx *intr_ctx,
					  unsigned long index)
{
	return xa_load(&intr_ctx->ctx, index);
}

static void vfio_irq_ctx_free(struct vfio_pci_intr_ctx *intr_ctx,
			      struct vfio_pci_irq_ctx *ctx, unsigned long index)
{
	xa_erase(&intr_ctx->ctx, index);
	kfree(ctx);
}

static struct vfio_pci_irq_ctx *
vfio_irq_ctx_alloc(struct vfio_pci_intr_ctx *intr_ctx, unsigned long index)
{
	struct vfio_pci_irq_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL_ACCOUNT);
	if (!ctx)
		return NULL;

	if (intr_ctx->ops->init_irq_ctx) {
		ret = intr_ctx->ops->init_irq_ctx(intr_ctx, ctx);
		if (ret < 0) {
			kfree(ctx);
			return NULL;
		}
	}

	ret = xa_insert(&intr_ctx->ctx, index, ctx, GFP_KERNEL_ACCOUNT);
	if (ret) {
		kfree(ctx);
		return NULL;
	}

	return ctx;
}

/*
 * INTx
 */
static void vfio_send_intx_eventfd(void *opaque, void *unused)
{
	struct vfio_pci_core_device *vdev = opaque;

	if (likely(is_intx(vdev) && !vdev->virq_disabled)) {
		struct vfio_pci_irq_ctx *ctx;

		ctx = vfio_irq_ctx_get(&vdev->intr_ctx, 0);
		if (WARN_ON_ONCE(!ctx))
			return;
		eventfd_signal(ctx->trigger, 1);
	}
}

/* Returns true if the INTx vfio_pci_irq_ctx.masked value is changed. */
bool vfio_pci_intx_mask(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_irq_ctx *ctx;
	unsigned long flags;
	bool masked_changed = false;

	spin_lock_irqsave(&vdev->irqlock, flags);

	/*
	 * Masking can come from interrupt, ioctl, or config space
	 * via INTx disable.  The latter means this can get called
	 * even when not using intx delivery.  In this case, just
	 * try to have the physical bit follow the virtual bit.
	 */
	if (unlikely(!is_intx(vdev))) {
		if (vdev->pci_2_3)
			pci_intx(pdev, 0);
		goto out_unlock;
	}

	ctx = vfio_irq_ctx_get(&vdev->intr_ctx, 0);
	if (WARN_ON_ONCE(!ctx))
		goto out_unlock;

	if (!ctx->masked) {
		/*
		 * Can't use check_and_mask here because we always want to
		 * mask, not just when something is pending.
		 */
		if (vdev->pci_2_3)
			pci_intx(pdev, 0);
		else
			disable_irq_nosync(pdev->irq);

		ctx->masked = true;
		masked_changed = true;
	}

out_unlock:
	spin_unlock_irqrestore(&vdev->irqlock, flags);
	return masked_changed;
}

/*
 * If this is triggered by an eventfd, we can't call eventfd_signal
 * or else we'll deadlock on the eventfd wait queue.  Return >0 when
 * a signal is necessary, which can then be handled via a work queue
 * or directly depending on the caller.
 */
static int vfio_pci_intx_unmask_handler(void *opaque, void *unused)
{
	struct vfio_pci_core_device *vdev = opaque;
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_irq_ctx *ctx;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&vdev->irqlock, flags);

	/*
	 * Unmasking comes from ioctl or config, so again, have the
	 * physical bit follow the virtual even when not using INTx.
	 */
	if (unlikely(!is_intx(vdev))) {
		if (vdev->pci_2_3)
			pci_intx(pdev, 1);
		goto out_unlock;
	}

	ctx = vfio_irq_ctx_get(&vdev->intr_ctx, 0);
	if (WARN_ON_ONCE(!ctx))
		goto out_unlock;

	if (ctx->masked && !vdev->virq_disabled) {
		/*
		 * A pending interrupt here would immediately trigger,
		 * but we can avoid that overhead by just re-sending
		 * the interrupt to the user.
		 */
		if (vdev->pci_2_3) {
			if (!pci_check_and_unmask_intx(pdev))
				ret = 1;
		} else
			enable_irq(pdev->irq);

		ctx->masked = (ret > 0);
	}

out_unlock:
	spin_unlock_irqrestore(&vdev->irqlock, flags);

	return ret;
}

void vfio_pci_intx_unmask(struct vfio_pci_core_device *vdev)
{
	if (vfio_pci_intx_unmask_handler(vdev, NULL) > 0)
		vfio_send_intx_eventfd(vdev, NULL);
}

static irqreturn_t vfio_intx_handler(int irq, void *dev_id)
{
	struct vfio_pci_core_device *vdev = dev_id;
	struct vfio_pci_irq_ctx *ctx;
	unsigned long flags;
	int ret = IRQ_NONE;

	ctx = vfio_irq_ctx_get(&vdev->intr_ctx, 0);
	if (WARN_ON_ONCE(!ctx))
		return ret;

	spin_lock_irqsave(&vdev->irqlock, flags);

	if (!vdev->pci_2_3) {
		disable_irq_nosync(vdev->pdev->irq);
		ctx->masked = true;
		ret = IRQ_HANDLED;
	} else if (!ctx->masked &&  /* may be shared */
		   pci_check_and_mask_intx(vdev->pdev)) {
		ctx->masked = true;
		ret = IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&vdev->irqlock, flags);

	if (ret == IRQ_HANDLED)
		vfio_send_intx_eventfd(vdev, NULL);

	return ret;
}

static int vfio_intx_enable(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_irq_ctx *ctx;

	if (!is_irq_none(&vdev->intr_ctx))
		return -EINVAL;

	if (!vdev->pdev->irq)
		return -ENODEV;

	ctx = vfio_irq_ctx_alloc(&vdev->intr_ctx, 0);
	if (!ctx)
		return -ENOMEM;

	/*
	 * If the virtual interrupt is masked, restore it.  Devices
	 * supporting DisINTx can be masked at the hardware level
	 * here, non-PCI-2.3 devices will have to wait until the
	 * interrupt is enabled.
	 */
	ctx->masked = vdev->virq_disabled;
	if (vdev->pci_2_3)
		pci_intx(vdev->pdev, !ctx->masked);

	vdev->intr_ctx.irq_type = VFIO_PCI_INTX_IRQ_INDEX;

	return 0;
}

static int vfio_intx_set_signal(struct vfio_pci_core_device *vdev, int fd)
{
	struct pci_dev *pdev = vdev->pdev;
	unsigned long irqflags = IRQF_SHARED;
	struct vfio_pci_irq_ctx *ctx;
	struct eventfd_ctx *trigger;
	unsigned long flags;
	int ret;

	ctx = vfio_irq_ctx_get(&vdev->intr_ctx, 0);
	if (WARN_ON_ONCE(!ctx))
		return -EINVAL;

	if (ctx->trigger) {
		free_irq(pdev->irq, vdev);
		kfree(ctx->name);
		eventfd_ctx_put(ctx->trigger);
		ctx->trigger = NULL;
	}

	if (fd < 0) /* Disable only */
		return 0;

	ctx->name = kasprintf(GFP_KERNEL_ACCOUNT, "vfio-intx(%s)",
			      pci_name(pdev));
	if (!ctx->name)
		return -ENOMEM;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger)) {
		kfree(ctx->name);
		return PTR_ERR(trigger);
	}

	ctx->trigger = trigger;

	if (!vdev->pci_2_3)
		irqflags = 0;

	ret = request_irq(pdev->irq, vfio_intx_handler,
			  irqflags, ctx->name, vdev);
	if (ret) {
		ctx->trigger = NULL;
		kfree(ctx->name);
		eventfd_ctx_put(trigger);
		return ret;
	}

	/*
	 * INTx disable will stick across the new irq setup,
	 * disable_irq won't.
	 */
	spin_lock_irqsave(&vdev->irqlock, flags);
	if (!vdev->pci_2_3 && ctx->masked)
		disable_irq_nosync(pdev->irq);
	spin_unlock_irqrestore(&vdev->irqlock, flags);

	return 0;
}

static void vfio_intx_disable(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_irq_ctx *ctx;

	ctx = vfio_irq_ctx_get(&vdev->intr_ctx, 0);
	WARN_ON_ONCE(!ctx);
	if (ctx) {
		vfio_virqfd_disable(&ctx->unmask);
		vfio_virqfd_disable(&ctx->mask);
	}
	vfio_intx_set_signal(vdev, -1);
	vdev->intr_ctx.irq_type = VFIO_PCI_NUM_IRQS;
	vfio_irq_ctx_free(&vdev->intr_ctx, ctx, 0);
}

/*
 * MSI/MSI-X
 */
static irqreturn_t vfio_msihandler(int irq, void *arg)
{
	struct eventfd_ctx *trigger = arg;

	eventfd_signal(trigger, 1);
	return IRQ_HANDLED;
}

static int vfio_msi_enable(struct vfio_pci_intr_ctx *intr_ctx, int nvec,
			   unsigned int index)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;
	struct pci_dev *pdev = vdev->pdev;
	unsigned int flag;
	int ret;
	u16 cmd;

	if (!is_irq_none(intr_ctx))
		return -EINVAL;

	flag = (index == VFIO_PCI_MSIX_IRQ_INDEX) ? PCI_IRQ_MSIX : PCI_IRQ_MSI;

	/* return the number of supported vectors if we can't get all: */
	cmd = vfio_pci_memory_lock_and_enable(vdev);
	ret = pci_alloc_irq_vectors(pdev, 1, nvec, flag);
	if (ret < nvec) {
		if (ret > 0)
			pci_free_irq_vectors(pdev);
		vfio_pci_memory_unlock_and_restore(vdev, cmd);
		return ret;
	}
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	intr_ctx->irq_type = index;

	if (index == VFIO_PCI_MSI_IRQ_INDEX) {
		/*
		 * Compute the virtual hardware field for max msi vectors -
		 * it is the log base 2 of the number of vectors.
		 */
		vdev->msi_qmax = fls(nvec * 2 - 1) - 1;
	}

	return 0;
}

/*
 * vfio_msi_alloc_irq() returns the Linux IRQ number of an MSI or MSI-X device
 * interrupt vector. If a Linux IRQ number is not available then a new
 * interrupt is allocated if dynamic MSI-X is supported.
 *
 * Where is vfio_msi_free_irq()? Allocated interrupts are maintained,
 * essentially forming a cache that subsequent allocations can draw from.
 * Interrupts are freed using pci_free_irq_vectors() when MSI/MSI-X is
 * disabled.
 */
static int vfio_msi_alloc_irq(struct vfio_pci_core_device *vdev,
			      unsigned int vector, bool msix)
{
	struct pci_dev *pdev = vdev->pdev;
	struct msi_map map;
	int irq;
	u16 cmd;

	irq = pci_irq_vector(pdev, vector);
	if (WARN_ON_ONCE(irq == 0))
		return -EINVAL;
	if (irq > 0 || !msix || !vdev->has_dyn_msix)
		return irq;

	cmd = vfio_pci_memory_lock_and_enable(vdev);
	map = pci_msix_alloc_irq_at(pdev, vector, NULL);
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	return map.index < 0 ? map.index : map.virq;
}

static void vfio_msi_free_interrupt(struct vfio_pci_intr_ctx *intr_ctx,
				    struct vfio_pci_irq_ctx *ctx,
				    unsigned int vector)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;
	u16 cmd;

	cmd = vfio_pci_memory_lock_and_enable(vdev);
	free_irq(ctx->virq, ctx->trigger);
	vfio_pci_memory_unlock_and_restore(vdev, cmd);
	ctx->virq = 0;
	/* Interrupt stays allocated, will be freed at MSI-X disable. */
}

static int vfio_msi_request_interrupt(struct vfio_pci_intr_ctx *intr_ctx,
				      struct vfio_pci_irq_ctx *ctx,
				      unsigned int vector,
				      unsigned int index)
{
	bool msix = (index == VFIO_PCI_MSIX_IRQ_INDEX) ? true : false;
	struct vfio_pci_core_device *vdev = intr_ctx->priv;
	int irq, ret;
	u16 cmd;

	/* Interrupt stays allocated, will be freed at MSI-X disable. */
	irq = vfio_msi_alloc_irq(vdev, vector, msix);
	if (irq < 0)
		return irq;

	/*
	 * If the vector was previously allocated, refresh the on-device
	 * message data before enabling in case it had been cleared or
	 * corrupted (e.g. due to backdoor resets) since writing.
	 */
	cmd = vfio_pci_memory_lock_and_enable(vdev);
	if (msix) {
		struct msi_msg msg;

		get_cached_msi_msg(irq, &msg);
		pci_write_msi_msg(irq, &msg);
	}

	ret = request_irq(irq, vfio_msihandler, 0, ctx->name, ctx->trigger);
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	ctx->virq = irq;

	return ret;
}

static char *vfio_msi_device_name(struct vfio_pci_intr_ctx *intr_ctx,
				  unsigned int vector,
				  unsigned int index)
{
	bool msix = (index == VFIO_PCI_MSIX_IRQ_INDEX) ? true : false;
	struct vfio_pci_core_device *vdev = intr_ctx->priv;
	struct pci_dev *pdev = vdev->pdev;

	return kasprintf(GFP_KERNEL_ACCOUNT, "vfio-msi%s[%d](%s)",
			 msix ? "x" : "", vector, pci_name(pdev));
}

static int vfio_msi_set_vector_signal(struct vfio_pci_intr_ctx *intr_ctx,
				      unsigned int vector, int fd,
				      unsigned int index)
{
	struct vfio_pci_irq_ctx *ctx;
	struct eventfd_ctx *trigger;
	int ret;

	ctx = vfio_irq_ctx_get(intr_ctx, vector);

	if (ctx && ctx->trigger) {
		if (!ctx->emulated) {
			irq_bypass_unregister_producer(&ctx->producer);
			intr_ctx->ops->msi_free_interrupt(intr_ctx, ctx, vector);
		}
		kfree(ctx->name);
		ctx->name = NULL;
		eventfd_ctx_put(ctx->trigger);
		ctx->trigger = NULL;
	}

	if (fd < 0)
		return 0;

	/* Per-interrupt context remain allocated. */
	if (!ctx) {
		ctx = vfio_irq_ctx_alloc(intr_ctx, vector);
		if (!ctx)
			return -ENOMEM;
	}

	ctx->name = intr_ctx->ops->msi_device_name(intr_ctx, vector, index);
	if (!ctx->name)
		return -ENOMEM;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger)) {
		ret = PTR_ERR(trigger);
		goto out_free_name;
	}

	ctx->trigger = trigger;

	if (ctx->emulated)
		return 0;

	ret = intr_ctx->ops->msi_request_interrupt(intr_ctx, ctx, vector, index);
	if (ret)
		goto out_put_eventfd_ctx;

	ctx->producer.token = trigger;
	ctx->producer.irq = ctx->virq;
	ret = irq_bypass_register_producer(&ctx->producer);
	if (unlikely(ret)) {
		pr_info("%s irq bypass producer (token %p) registration fails: %d\n",
			ctx->name, ctx->producer.token, ret);

		ctx->producer.token = NULL;
	}

	return 0;

out_put_eventfd_ctx:
	eventfd_ctx_put(ctx->trigger);
	ctx->trigger = NULL;
out_free_name:
	kfree(ctx->name);
	ctx->name = NULL;
	return ret;
}

static int vfio_msi_set_block(struct vfio_pci_intr_ctx *intr_ctx,
			      unsigned int start, unsigned int count,
			      int32_t *fds, unsigned int index)
{
	unsigned int i, j;
	int ret = 0;

	for (i = 0, j = start; i < count && !ret; i++, j++) {
		int fd = fds ? fds[i] : -1;
		ret = vfio_msi_set_vector_signal(intr_ctx, j, fd, index);
	}

	if (ret) {
		for (i = start; i < j; i++)
			vfio_msi_set_vector_signal(intr_ctx, i, -1, index);
	}

	return ret;
}

static void vfio_msi_disable(struct vfio_pci_intr_ctx *intr_ctx,
			     unsigned int index)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_irq_ctx *ctx;
	unsigned long i;
	u16 cmd;

	xa_for_each(&intr_ctx->ctx, i, ctx) {
		vfio_virqfd_disable(&ctx->unmask);
		vfio_virqfd_disable(&ctx->mask);
		vfio_msi_set_vector_signal(intr_ctx, i, -1, index);
	}

	cmd = vfio_pci_memory_lock_and_enable(vdev);
	pci_free_irq_vectors(pdev);
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	/*
	 * Both disable paths above use pci_intx_for_msi() to clear DisINTx
	 * via their shutdown paths.  Restore for NoINTx devices.
	 */
	if (vdev->nointx)
		pci_intx(pdev, 0);

	intr_ctx->irq_type = VFIO_PCI_NUM_IRQS;
}

/*
 * IOCTL support
 */
static int vfio_pci_set_intx_unmask(struct vfio_pci_intr_ctx *intr_ctx,
				    unsigned int index, unsigned int start,
				    unsigned int count, uint32_t flags,
				    void *data)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;

	if (!is_intx(vdev) || start != 0 || count != 1)
		return -EINVAL;

	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		vfio_pci_intx_unmask(vdev);
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		uint8_t unmask = *(uint8_t *)data;
		if (unmask)
			vfio_pci_intx_unmask(vdev);
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		struct vfio_pci_irq_ctx *ctx = vfio_irq_ctx_get(&vdev->intr_ctx,
								0);
		int32_t fd = *(int32_t *)data;

		if (WARN_ON_ONCE(!ctx))
			return -EINVAL;
		if (fd >= 0)
			return vfio_virqfd_enable((void *) vdev,
						  vfio_pci_intx_unmask_handler,
						  vfio_send_intx_eventfd, NULL,
						  &ctx->unmask, fd);

		vfio_virqfd_disable(&ctx->unmask);
	}

	return 0;
}

static int vfio_pci_set_intx_mask(struct vfio_pci_intr_ctx *intr_ctx,
				  unsigned int index, unsigned int start,
				  unsigned int count, uint32_t flags, void *data)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;

	if (!is_intx(vdev) || start != 0 || count != 1)
		return -EINVAL;

	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		vfio_pci_intx_mask(vdev);
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		uint8_t mask = *(uint8_t *)data;
		if (mask)
			vfio_pci_intx_mask(vdev);
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		return -ENOTTY; /* XXX implement me */
	}

	return 0;
}

static int vfio_pci_set_intx_trigger(struct vfio_pci_intr_ctx *intr_ctx,
				     unsigned int index, unsigned int start,
				     unsigned int count, uint32_t flags,
				     void *data)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;

	if (is_intx(vdev) && !count && (flags & VFIO_IRQ_SET_DATA_NONE)) {
		vfio_intx_disable(vdev);
		return 0;
	}

	if (!(is_intx(vdev) || is_irq_none(intr_ctx)) || start != 0 || count != 1)
		return -EINVAL;

	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		int32_t fd = *(int32_t *)data;
		int ret;

		if (is_intx(vdev))
			return vfio_intx_set_signal(vdev, fd);

		ret = vfio_intx_enable(vdev);
		if (ret)
			return ret;

		ret = vfio_intx_set_signal(vdev, fd);
		if (ret)
			vfio_intx_disable(vdev);

		return ret;
	}

	if (!is_intx(vdev))
		return -EINVAL;

	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		vfio_send_intx_eventfd(vdev, NULL);
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		uint8_t trigger = *(uint8_t *)data;
		if (trigger)
			vfio_send_intx_eventfd(vdev, NULL);
	}
	return 0;
}

static int vfio_pci_set_msi_trigger(struct vfio_pci_intr_ctx *intr_ctx,
				    unsigned int index, unsigned int start,
				    unsigned int count, uint32_t flags,
				    void *data)
{
	struct vfio_pci_irq_ctx *ctx;
	unsigned int i;

	if (irq_is(intr_ctx, index) && !count && (flags & VFIO_IRQ_SET_DATA_NONE)) {
		intr_ctx->ops->msi_disable(intr_ctx, index);
		return 0;
	}

	if (!(irq_is(intr_ctx, index) || is_irq_none(intr_ctx)))
		return -EINVAL;

	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		int32_t *fds = data;
		int ret;

		if (intr_ctx->irq_type == index)
			return vfio_msi_set_block(intr_ctx, start, count,
						  fds, index);

		ret = intr_ctx->ops->msi_enable(intr_ctx, start + count, index);
		if (ret)
			return ret;

		ret = vfio_msi_set_block(intr_ctx, start, count, fds, index);
		if (ret)
			intr_ctx->ops->msi_disable(intr_ctx, index);

		return ret;
	}

	if (!irq_is(intr_ctx, index))
		return -EINVAL;

	for (i = start; i < start + count; i++) {
		ctx = vfio_irq_ctx_get(intr_ctx, i);
		if (!ctx || !ctx->trigger)
			continue;
		if (flags & VFIO_IRQ_SET_DATA_NONE) {
			eventfd_signal(ctx->trigger, 1);
		} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
			uint8_t *bools = data;
			if (bools[i - start])
				eventfd_signal(ctx->trigger, 1);
		}
	}
	return 0;
}

static int vfio_pci_set_ctx_trigger_single(struct eventfd_ctx **ctx,
					   unsigned int count, uint32_t flags,
					   void *data)
{
	/* DATA_NONE/DATA_BOOL enables loopback testing */
	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		if (*ctx) {
			if (count) {
				eventfd_signal(*ctx, 1);
			} else {
				eventfd_ctx_put(*ctx);
				*ctx = NULL;
			}
			return 0;
		}
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		uint8_t trigger;

		if (!count)
			return -EINVAL;

		trigger = *(uint8_t *)data;
		if (trigger && *ctx)
			eventfd_signal(*ctx, 1);

		return 0;
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		int32_t fd;

		if (!count)
			return -EINVAL;

		fd = *(int32_t *)data;
		if (fd == -1) {
			if (*ctx)
				eventfd_ctx_put(*ctx);
			*ctx = NULL;
		} else if (fd >= 0) {
			struct eventfd_ctx *efdctx;

			efdctx = eventfd_ctx_fdget(fd);
			if (IS_ERR(efdctx))
				return PTR_ERR(efdctx);

			if (*ctx)
				eventfd_ctx_put(*ctx);

			*ctx = efdctx;
		}
		return 0;
	}

	return -EINVAL;
}

static int vfio_pci_set_err_trigger(struct vfio_pci_intr_ctx *intr_ctx,
				    unsigned int index, unsigned int start,
				    unsigned int count, uint32_t flags,
				    void *data)
{
	struct vfio_pci_core_device *vdev = intr_ctx->priv;

	if (!pci_is_pcie(vdev->pdev))
		return -ENOTTY;

	if (index != VFIO_PCI_ERR_IRQ_INDEX || start != 0 || count > 1)
		return -EINVAL;

	return vfio_pci_set_ctx_trigger_single(&intr_ctx->err_trigger,
					       count, flags, data);
}

static int vfio_pci_set_req_trigger(struct vfio_pci_intr_ctx *intr_ctx,
				    unsigned int index, unsigned int start,
				    unsigned int count, uint32_t flags,
				    void *data)
{
	if (index != VFIO_PCI_REQ_IRQ_INDEX || start != 0 || count > 1)
		return -EINVAL;

	return vfio_pci_set_ctx_trigger_single(&intr_ctx->req_trigger,
					       count, flags, data);
}

static void _vfio_pci_init_intr_ctx(struct vfio_pci_intr_ctx *intr_ctx)
{
	intr_ctx->irq_type = VFIO_PCI_NUM_IRQS;
	mutex_init(&intr_ctx->igate);
	xa_init(&intr_ctx->ctx);
}

static void _vfio_pci_release_intr_ctx(struct vfio_pci_intr_ctx *intr_ctx)
{
	struct vfio_pci_irq_ctx *ctx;
	unsigned long i;

	/*
	 * Per-interrupt context remains allocated after interrupt is
	 * freed. Per-interrupt context need to be freed separately.
	 */
	mutex_lock(&intr_ctx->igate);
	xa_for_each(&intr_ctx->ctx, i, ctx) {
		WARN_ON_ONCE(ctx->trigger);
		WARN_ON_ONCE(ctx->name);
		xa_erase(&intr_ctx->ctx, i);
		kfree(ctx);
	}
	mutex_unlock(&intr_ctx->igate);

	mutex_destroy(&intr_ctx->igate);
}

static struct vfio_pci_intr_ops vfio_pci_intr_ops = {
	.set_intx_mask = vfio_pci_set_intx_mask,
	.set_intx_unmask = vfio_pci_set_intx_unmask,
	.set_intx_trigger = vfio_pci_set_intx_trigger,
	.set_msi_trigger = vfio_pci_set_msi_trigger,
	.set_msix_trigger = vfio_pci_set_msi_trigger,
	.set_err_trigger = vfio_pci_set_err_trigger,
	.set_req_trigger = vfio_pci_set_req_trigger,
	.msi_enable = vfio_msi_enable,
	.msi_disable = vfio_msi_disable,
	.msi_request_interrupt = vfio_msi_request_interrupt,
	.msi_free_interrupt = vfio_msi_free_interrupt,
	.msi_device_name = vfio_msi_device_name,
};

void vfio_pci_init_intr_ctx(struct vfio_pci_core_device *vdev,
			    struct vfio_pci_intr_ctx *intr_ctx)
{
	_vfio_pci_init_intr_ctx(intr_ctx);
	intr_ctx->ops = &vfio_pci_intr_ops;
	intr_ctx->priv = vdev;
	intr_ctx->ims_backed_irq = false;
}
EXPORT_SYMBOL_GPL(vfio_pci_init_intr_ctx);

void vfio_pci_release_intr_ctx(struct vfio_pci_intr_ctx *intr_ctx)
{
	_vfio_pci_release_intr_ctx(intr_ctx);
}
EXPORT_SYMBOL_GPL(vfio_pci_release_intr_ctx);

/*
 * vfio_pci_send_signal() - Send signal to the eventfd.
 * @intr_ctx:	Interrupt context.
 * @vector:	Vector for which interrupt will be signaled.
 *
 * Trigger signal to guest for emulated interrupts.
 */
void vfio_pci_send_signal(struct vfio_pci_intr_ctx *intr_ctx, unsigned int vector)
{
	struct vfio_pci_irq_ctx *ctx;

	mutex_lock(&intr_ctx->igate);

	ctx = vfio_irq_ctx_get(intr_ctx, vector);

	if (WARN_ON_ONCE(!ctx || !ctx->emulated || !ctx->trigger))
		goto out_unlock;

	eventfd_signal(ctx->trigger, 1);

out_unlock:
	mutex_unlock(&intr_ctx->igate);
}
EXPORT_SYMBOL_GPL(vfio_pci_send_signal);

/*
 * vfio_pci_set_emulated() - Set range of interrupts that will be emulated.
 * @intr_ctx:	Interrupt context.
 * @start:	First emulated interrupt vector.
 * @count:	Number of emulated interrupts starting from @start.
 *
 * Emulated interrupts will not be backed by hardware interrupts but
 * instead triggered by virtual device driver.
 *
 * Return: error code on failure (-EBUSY if the vector is not available,
 * -ENOMEM on allocation failure), 0 on success. No partial success, on
 * success entire range was set as emulated, on failure no interrupt in
 * range was set as emulated.
 */
int vfio_pci_set_emulated(struct vfio_pci_intr_ctx *intr_ctx,
			  unsigned int start, unsigned int count)
{
	struct vfio_pci_irq_ctx *ctx;
	unsigned long i, j;
	int ret = -EINVAL;

	mutex_lock(&intr_ctx->igate);

	for (i = start; i < start + count; i++) {
		ctx = kzalloc(sizeof(*ctx), GFP_KERNEL_ACCOUNT);
		if (!ctx) {
			ret = -ENOMEM;
			goto out_err;
		}
		ctx->emulated = true;
		ret = xa_insert(&intr_ctx->ctx, i, ctx, GFP_KERNEL_ACCOUNT);
		if (ret) {
			kfree(ctx);
			goto out_err;
		}
	}

	mutex_unlock(&intr_ctx->igate);
	return 0;

out_err:
	for (j = start; j < i; j++) {
		ctx = vfio_irq_ctx_get(intr_ctx, j);
		vfio_irq_ctx_free(intr_ctx, ctx, j);
	}

	mutex_unlock(&intr_ctx->igate);

	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_set_emulated);

/* Guest MSI-X interrupts backed by IMS host interrupts */

/*
 * Free the IMS interrupt associated with @ctx.
 *
 * For an IMS interrupt the interrupt is freed from the underlying
 * PCI device's IMS domain.
 */
static void vfio_pci_ims_irq_free(struct vfio_pci_intr_ctx *intr_ctx,
				  struct vfio_pci_irq_ctx *ctx)
{
	struct vfio_pci_ims *ims = intr_ctx->priv;
	struct msi_map irq_map = {};

	irq_map.index = ctx->ims_id;
	irq_map.virq = ctx->virq;
	pci_ims_free_irq(ims->pdev, irq_map);
	ctx->ims_id = -EINVAL;
	ctx->virq = 0;
}

/*
 * Allocate a host IMS interrupt for @ctx.
 *
 * For an IMS interrupt the interrupt is allocated from the underlying
 * PCI device's IMS domain.
 */
static int vfio_pci_ims_irq_alloc(struct vfio_pci_intr_ctx *intr_ctx,
				  struct vfio_pci_irq_ctx *ctx)
{
	struct vfio_pci_ims *ims = intr_ctx->priv;
	struct msi_map irq_map = {};

	irq_map = pci_ims_alloc_irq(ims->pdev, &ctx->icookie, NULL);
	if (irq_map.index < 0)
		return irq_map.index;

	ctx->ims_id = irq_map.index;
	ctx->virq = irq_map.virq;

	return 0;
}

static void vfio_ims_free_interrupt(struct vfio_pci_intr_ctx *intr_ctx,
				    struct vfio_pci_irq_ctx *ctx,
				    unsigned int vector)
{
	free_irq(ctx->virq, ctx->trigger);
	vfio_pci_ims_irq_free(intr_ctx, ctx);
}

static int vfio_ims_request_interrupt(struct vfio_pci_intr_ctx *intr_ctx,
				      struct vfio_pci_irq_ctx *ctx,
				      unsigned int vector,
				      unsigned int index)
{
	int ret;

	ret = vfio_pci_ims_irq_alloc(intr_ctx, ctx);
	if (ret < 0)
		return ret;

	ret = request_irq(ctx->virq, vfio_msihandler, 0, ctx->name,
			  ctx->trigger);
	if (ret < 0) {
		vfio_pci_ims_irq_free(intr_ctx, ctx);
		return ret;
	}

	return 0;
}

static char *vfio_ims_device_name(struct vfio_pci_intr_ctx *intr_ctx,
				  unsigned int vector,
				  unsigned int index)
{
	struct vfio_pci_ims *ims = intr_ctx->priv;
	struct device *dev = &ims->vdev->device;

	return kasprintf(GFP_KERNEL, "vfio-ims[%d](%s)", vector, dev_name(dev));
}

static void vfio_ims_disable(struct vfio_pci_intr_ctx *intr_ctx,
			     unsigned int index)
{
	struct vfio_pci_irq_ctx *ctx;
	unsigned long i;

	xa_for_each(&intr_ctx->ctx, i, ctx)
		vfio_msi_set_vector_signal(intr_ctx, i, -1, index);
}

/*
 * The virtual device driver is responsible for enabling IMS by creating
 * the IMS domaim from where interrupts will be allocated dynamically.
 * IMS thus has to be enabled by the time an ioctl() arrives.
 */
static int vfio_ims_enable(struct vfio_pci_intr_ctx *intr_ctx, int nvec,
			   unsigned int index)
{
	return -EINVAL;
}

static int vfio_ims_init_irq_ctx(struct vfio_pci_intr_ctx *intr_ctx,
				 struct vfio_pci_irq_ctx *ctx)
{
	struct vfio_pci_ims *ims = intr_ctx->priv;

	ctx->icookie = ims->default_cookie;

	return 0;
}

static struct vfio_pci_intr_ops vfio_pci_ims_intr_ops = {
	.set_msix_trigger = vfio_pci_set_msi_trigger,
	.set_req_trigger = vfio_pci_set_req_trigger,
	.msi_enable = vfio_ims_enable,
	.msi_disable = vfio_ims_disable,
	.msi_request_interrupt = vfio_ims_request_interrupt,
	.msi_free_interrupt = vfio_ims_free_interrupt,
	.msi_device_name = vfio_ims_device_name,
	.init_irq_ctx = vfio_ims_init_irq_ctx,
};

int vfio_pci_ims_init_intr_ctx(struct vfio_device *vdev,
			       struct vfio_pci_intr_ctx *intr_ctx,
			       struct pci_dev *pdev,
			       union msi_instance_cookie *default_cookie)
{
	struct vfio_pci_ims *ims;

	ims = kzalloc(sizeof(*ims), GFP_KERNEL_ACCOUNT);
	if (!ims)
		return -ENOMEM;

	ims->pdev = pdev;
	ims->default_cookie = *default_cookie;
	ims->vdev = vdev;

	_vfio_pci_init_intr_ctx(intr_ctx);

	intr_ctx->ops = &vfio_pci_ims_intr_ops;
	intr_ctx->priv = ims;
	intr_ctx->ims_backed_irq = true;
	intr_ctx->irq_type = VFIO_PCI_MSIX_IRQ_INDEX;

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_ims_init_intr_ctx);

void vfio_pci_ims_release_intr_ctx(struct vfio_pci_intr_ctx *intr_ctx)
{
	struct vfio_pci_ims *ims = intr_ctx->priv;

	_vfio_pci_release_intr_ctx(intr_ctx);
	kfree(ims);
	intr_ctx->irq_type = VFIO_PCI_NUM_IRQS;
}
EXPORT_SYMBOL_GPL(vfio_pci_ims_release_intr_ctx);

/*
 * Return IMS index of IMS interrupt backing MSI-X interrupt @vector
 */
int vfio_pci_ims_hwirq(struct vfio_pci_intr_ctx *intr_ctx, unsigned int vector)
{
	struct vfio_pci_irq_ctx *ctx;
	int id = -EINVAL;

	mutex_lock(&intr_ctx->igate);

	if (!intr_ctx->ims_backed_irq)
		goto out_unlock;

	ctx = vfio_irq_ctx_get(intr_ctx, vector);
	if (!ctx || ctx->emulated)
		goto out_unlock;

	id = ctx->ims_id;

out_unlock:
	mutex_unlock(&intr_ctx->igate);
	return id;
}
EXPORT_SYMBOL_GPL(vfio_pci_ims_hwirq);

/*
 * vfio_pci_ims_set_cookie() - Set unique cookie for vector.
 * @intr_ctx:	Interrupt context.
 * @vector:	Vector.
 * @icookie:	New cookie for @vector.
 *
 * When new IMS interrupt is allocated for @vector it will be
 * assigned @icookie.
 */
int vfio_pci_ims_set_cookie(struct vfio_pci_intr_ctx *intr_ctx,
			    unsigned int vector,
			    union msi_instance_cookie *icookie)
{
	struct vfio_pci_irq_ctx *ctx;
	int ret = -EINVAL;

	mutex_lock(&intr_ctx->igate);

	if (!intr_ctx->ims_backed_irq)
		goto out_unlock;

	ctx = vfio_irq_ctx_get(intr_ctx, vector);
	if (ctx) {
		if (WARN_ON_ONCE(ctx->emulated)) {
			ret = -EINVAL;
			goto out_unlock;
		}
		ctx->icookie = *icookie;
		ret = 0;
		goto out_unlock;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL_ACCOUNT);
	if (!ctx) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	ctx->icookie = *icookie;
	ret = xa_insert(&intr_ctx->ctx, vector, ctx, GFP_KERNEL_ACCOUNT);
	if (ret) {
		kfree(ctx);
		goto out_unlock;
	}

	ret = 0;

out_unlock:
	mutex_unlock(&intr_ctx->igate);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_ims_set_cookie);

int vfio_pci_set_irqs_ioctl(struct vfio_pci_intr_ctx *intr_ctx, uint32_t flags,
			    unsigned int index, unsigned int start,
			    unsigned int count, void *data)
{
	int (*func)(struct vfio_pci_intr_ctx *intr_ctx, unsigned int index,
		    unsigned int start, unsigned int count, uint32_t flags,
		    void *data) = NULL;
	int ret = -ENOTTY;

	mutex_lock(&intr_ctx->igate);
	switch (index) {
	case VFIO_PCI_INTX_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
			if (intr_ctx->ops->set_intx_mask)
				func = intr_ctx->ops->set_intx_mask;
			break;
		case VFIO_IRQ_SET_ACTION_UNMASK:
			if (intr_ctx->ops->set_intx_unmask)
				func = intr_ctx->ops->set_intx_unmask;
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			if (intr_ctx->ops->set_intx_trigger)
				func = intr_ctx->ops->set_intx_trigger;
			break;
		}
		break;
	case VFIO_PCI_MSI_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
		case VFIO_IRQ_SET_ACTION_UNMASK:
			/* XXX Need masking support exported */
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			if (intr_ctx->ops->set_msi_trigger)
				func = intr_ctx->ops->set_msi_trigger;
			break;
		}
		break;
	case VFIO_PCI_MSIX_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
		case VFIO_IRQ_SET_ACTION_UNMASK:
			/* XXX Need masking support exported */
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			if (intr_ctx->ops->set_msix_trigger)
				func = intr_ctx->ops->set_msix_trigger;
			break;
		}
		break;
	case VFIO_PCI_ERR_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			if (intr_ctx->ops->set_err_trigger)
				func = intr_ctx->ops->set_err_trigger;
			break;
		}
		break;
	case VFIO_PCI_REQ_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			if (intr_ctx->ops->set_req_trigger)
				func = intr_ctx->ops->set_req_trigger;
			break;
		}
		break;
	}

	if (!func)
		goto out_unlock;

	ret = func(intr_ctx, index, start, count, flags, data);

out_unlock:
	mutex_unlock(&intr_ctx->igate);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_set_irqs_ioctl);
