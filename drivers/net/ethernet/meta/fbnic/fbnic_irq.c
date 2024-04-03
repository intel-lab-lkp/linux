// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/pci.h>
#include <linux/types.h>

#include "fbnic.h"
#include "fbnic_txrx.h"

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

static irqreturn_t fbnic_mac_msix_intr(int __always_unused irq, void *data)
{
	struct fbnic_dev *fbd = data;

	if (fbd->mac->get_link_event(fbd))
		fbd->link_state = FBNIC_LINK_EVENT;
	else
		wr32(FBNIC_INTR_MASK_CLEAR(0), 1u << FBNIC_MAC_MSIX_ENTRY);

	return IRQ_HANDLED;
}

/**
 * fbnic_mac_get_link - Retrieve the current link state of the MAC
 * @fbd: Device to retrieve the link state of
 * @link: pointer to boolean value that will store link state
 *
 * This function will query the hardware to determine the state of the
 * hardware to determine the link status of the device. If it is unable to
 * communicate with the device it will return ENODEV and return false
 * indicating the link is down.
 **/
int fbnic_mac_get_link(struct fbnic_dev *fbd, bool *link)
{
	const struct fbnic_mac *mac = fbd->mac;

	*link = true;

	/* In an interrupt driven setup we can just skip the check if
	 * the link is up as the interrupt should toggle it to the EVENT
	 * state if the link has changed state at any time since the last
	 * check.
	 */
	if (fbd->link_state == FBNIC_LINK_UP)
		goto skip_check;

	*link = mac->get_link(fbd);

	wr32(FBNIC_INTR_MASK_CLEAR(0), 1u << FBNIC_MAC_MSIX_ENTRY);
skip_check:
	if (!fbnic_present(fbd)) {
		*link = false;
		return -ENODEV;
	}

	return 0;
}

/**
 * fbnic_mac_enable - Configure the MAC to enable it to advertise link
 * @fbd: Pointer to device to initialize
 *
 * This function provides basic bringup for the CMAC and sets the link
 * state to FBNIC_LINK_EVENT which tells the link state check that the
 * current state is unknown and that interrupts must be enabled after the
 * check is completed.
 **/
int fbnic_mac_enable(struct fbnic_dev *fbd)
{
	const struct fbnic_mac *mac = fbd->mac;
	u32 vector = fbd->mac_msix_vector;
	int err;

	/* Request the IRQ for MAC link vector.
	 * Map MAC cause to it, and unmask it
	 */
	err = request_irq(vector, &fbnic_mac_msix_intr, 0,
			  fbd->netdev->name, fbd);
	if (err)
		return err;

	wr32(FBNIC_INTR_MSIX_CTRL(FBNIC_INTR_MSIX_CTRL_PCS_IDX),
	     FBNIC_MAC_MSIX_ENTRY | FBNIC_INTR_MSIX_CTRL_ENABLE);

	err = mac->enable(fbd);
	if (err) {
		/* Disable interrupt */
		wr32(FBNIC_INTR_MSIX_CTRL(FBNIC_INTR_MSIX_CTRL_PCS_IDX),
		     FBNIC_MAC_MSIX_ENTRY);
		wr32(FBNIC_INTR_MASK_SET(0), 1u << FBNIC_MAC_MSIX_ENTRY);

		/* Free the vector */
		free_irq(fbd->mac_msix_vector, fbd);
	}

	return err;
}

/**
 * fbnic_mac_disable - Teardown the MAC to prepare for stopping
 * @fbd: Pointer to device that is stopping
 *
 * This function undoes the work done in fbnic_mac_enable and prepares the
 * device to no longer receive traffic on the host interface.
 **/
void fbnic_mac_disable(struct fbnic_dev *fbd)
{
	const struct fbnic_mac *mac = fbd->mac;

	/* Nothing to do if link is already disabled */
	if (fbd->link_state == FBNIC_LINK_DISABLED)
		return;

	mac->disable(fbd);

	/* Disable interrupt */
	wr32(FBNIC_INTR_MSIX_CTRL(FBNIC_INTR_MSIX_CTRL_PCS_IDX),
	     FBNIC_MAC_MSIX_ENTRY);
	wr32(FBNIC_INTR_MASK_SET(0), 1u << FBNIC_MAC_MSIX_ENTRY);

	/* Free the vector */
	free_irq(fbd->mac_msix_vector, fbd);
}

void fbnic_free_irqs(struct fbnic_dev *fbd)
{
	struct pci_dev *pdev = to_pci_dev(fbd->dev);

	fbd->mac_msix_vector = 0;
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

	wanted_irqs += min_t(unsigned int, num_online_cpus(), FBNIC_MAX_RXQS);
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

	fbd->mac_msix_vector = msix_entries[FBNIC_MAC_MSIX_ENTRY].vector;
	fbd->fw_msix_vector = msix_entries[FBNIC_FW_MSIX_ENTRY].vector;

	return 0;
}
