// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/pci.h>
#include <linux/types.h>

#include "fbnic.h"

static irqreturn_t fbnic_fw_msix_intr(int __always_unused irq, void *data)
{
	struct fbnic_dev *fbd = (struct fbnic_dev *)data;

	fbnic_mbx_poll(fbd);

	wr32(FBNIC_INTR_MASK_CLEAR(0), 1u << FBNIC_FW_MSIX_ENTRY);

	return IRQ_HANDLED;
}

/**
 * fbnic_fw_enable_mbx - Configure and initialize Firmware Mailbox
 * @fbd: Pointer to device to initialize
 *
 * This function will initialize the firmware mailbox rings, enable the IRQ
 * and initialize the communication between the Firmware and the host. The
 * firmware is expected to respond to the initialization by sending an
 * interrupt essentially notifying the host that it has seen the
 * initialization and is now synced up.
 **/
int fbnic_fw_enable_mbx(struct fbnic_dev *fbd)
{
	u32 vector;
	int err;

	vector = fbd->fw_msix_vector;

	/* Request the IRQ for MAC link vector.
	 * Map MAC cause to it, and unmask it
	 */
	err = request_threaded_irq(vector, NULL, &fbnic_fw_msix_intr, 0,
				   dev_name(fbd->dev), fbd);
	if (err)
		return err;

	/* Initialize mailbox and attempt to poll it into ready state */
	fbnic_mbx_init(fbd);
	err = fbnic_mbx_poll_tx_ready(fbd);
	if (err)
		dev_warn(fbd->dev, "FW mailbox did not enter ready state\n");

	/* Enable interrupts */
	wr32(FBNIC_INTR_SW_AC_MODE(0), ~(1u << FBNIC_FW_MSIX_ENTRY));
	wr32(FBNIC_INTR_MASK_CLEAR(0), 1u << FBNIC_FW_MSIX_ENTRY);

	return 0;
}

/**
 * fbnic_fw_disable_mbx - Disable mailbox and place it in standby state
 * @fbd: Pointer to device to disable
 *
 * This function will disable the mailbox interrupt, free any messages still
 * in the mailbox and place it into a standby state. The firmware is
 * expected to see the update and assume that the host is in the reset state.
 **/
void fbnic_fw_disable_mbx(struct fbnic_dev *fbd)
{
	/* Disable interrupt and free vector */
	wr32(FBNIC_INTR_MASK_SET(0), 1u << FBNIC_FW_MSIX_ENTRY);

	/* Re-enable auto-clear for the mailbox register */
	wr32(FBNIC_INTR_SW_AC_MODE(0), ~0);

	/* Free the vector */
	free_irq(fbd->fw_msix_vector, fbd);

	/* Make sure disabling logs message is sent, must be done here to
	 * avoid risk of completing without a running interrupt.
	 */
	fbnic_mbx_flush_tx(fbd);

	/* Flush any remaining entries */
	fbnic_mbx_clean(fbd);
}

void fbnic_free_irqs(struct fbnic_dev *fbd)
{
	struct pci_dev *pdev = to_pci_dev(fbd->dev);

	fbd->fw_msix_vector = 0;
	fbd->num_irqs = 0;

	pci_disable_msix(pdev);

	kfree(fbd->msix_entries);
	fbd->msix_entries = NULL;
}

int fbnic_alloc_irqs(struct fbnic_dev *fbd)
{
	unsigned int wanted_irqs = FBNIC_NON_NAPI_VECTORS;
	struct pci_dev *pdev = to_pci_dev(fbd->dev);
	struct msix_entry *msix_entries;
	int i, num_irqs;

	msix_entries = kcalloc(wanted_irqs, sizeof(*msix_entries), GFP_KERNEL);
	if (!msix_entries)
		return -ENOMEM;

	for (i = 0; i < wanted_irqs; i++)
		msix_entries[i].entry = i;

	num_irqs = pci_enable_msix_range(pdev, msix_entries,
					 FBNIC_NON_NAPI_VECTORS + 1,
					 wanted_irqs);
	if (num_irqs < 0) {
		dev_err(fbd->dev, "Failed to allocate MSI-X entries\n");
		kfree(msix_entries);
		return num_irqs;
	}

	if (num_irqs < wanted_irqs)
		dev_warn(fbd->dev, "Allocated %d IRQs, expected %d\n",
			 num_irqs, wanted_irqs);

	fbd->msix_entries = msix_entries;
	fbd->num_irqs = num_irqs;

	fbd->fw_msix_vector = msix_entries[FBNIC_FW_MSIX_ENTRY].vector;
	return 0;
}
