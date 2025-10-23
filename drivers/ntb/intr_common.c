// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)

#include <linux/module.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/slab.h>

int ntb_intr_init(struct ntb_dev *ntb,
		  void (*desc_changed)(void *ctx))
{
#ifdef CONFIG_NTB_MSI
	if (ntb->pdev->dev.msi.data) {
		ntb->intr_backend = ntb_intr_msi_backend();
		dev_info(&ntb->dev, "NTB interrupt MSI backend selected.\n");
	}
#endif
#ifdef CONFIG_NTB_DW_EDMA
	if (!ntb->intr_backend) {
		ntb->intr_backend = ntb_intr_dw_edma_backend();
		dev_info(&ntb->dev, "NTB interrupt DW eDMA backend selected.\n");
	}
#endif
	if (!ntb->intr_backend)
		return -ENODEV;
	return ntb->intr_backend->init(ntb, desc_changed);
}
EXPORT_SYMBOL_GPL(ntb_intr_init);

int ntb_intr_setup_mws(struct ntb_dev *ntb)
{
	return ntb->intr_backend->setup_mws(ntb);
}
EXPORT_SYMBOL_GPL(ntb_intr_setup_mws);

void ntb_intr_clear_mws(struct ntb_dev *ntb)
{
	ntb->intr_backend->clear_mws(ntb);
}
EXPORT_SYMBOL_GPL(ntb_intr_clear_mws);

int ntb_intr_request_irq(struct ntb_dev *ntb, irq_handler_t h,
			 const char *name, void *dev_id,
			 struct ntb_intr_desc *d)
{
	return ntb->intr_backend->request_irq(ntb, h, name, dev_id, d);
}
EXPORT_SYMBOL_GPL(ntb_intr_request_irq);

void ntb_intr_free_irq(struct ntb_dev *ntb, int irq, void *dev_id,
		       struct ntb_intr_desc *d)
{
	return ntb->intr_backend->free_irq(ntb, irq, dev_id, d);
}
EXPORT_SYMBOL_GPL(ntb_intr_free_irq);

int ntb_intr_peer_trigger(struct ntb_dev *ntb, int peer,
			  struct ntb_intr_desc *d)
{
	return ntb->intr_backend->peer_trigger(ntb, peer, d);
}
EXPORT_SYMBOL_GPL(ntb_intr_peer_trigger);
