// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2015 - 2026 Beijing WangXun Technology Co., Ltd. */

#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/aer.h>

#include "wx_type.h"
#include "wx_lib.h"
#include "wx_err.h"

/**
 * wx_io_error_detected - called when PCI error is detected
 * @pdev: Pointer to PCI device
 * @state: The current pci connection state
 *
 * Return: pci_ers_result_t.
 *
 * This function is called after a PCI bus error affecting
 * this device has been detected.
 */
static pci_ers_result_t wx_io_error_detected(struct pci_dev *pdev,
					     pci_channel_state_t state)
{
	struct wx *wx = pci_get_drvdata(pdev);
	struct net_device *netdev;

	netdev = wx->netdev;
	if (!netif_device_present(netdev))
		return PCI_ERS_RESULT_DISCONNECT;

	rtnl_lock();
	netif_device_detach(netdev);

	if (netif_running(netdev))
		wx->close_suspend(wx);

	if (state == pci_channel_io_perm_failure) {
		rtnl_unlock();
		return PCI_ERS_RESULT_DISCONNECT;
	}

	if (!test_and_set_bit(WX_STATE_DISABLED, wx->state))
		pci_disable_device(pdev);
	rtnl_unlock();

	/* Request a slot reset. */
	return PCI_ERS_RESULT_NEED_RESET;
}

/**
 * wx_io_slot_reset - called after the pci bus has been reset.
 * @pdev: Pointer to PCI device
 *
 * Return: pci_ers_result_t.
 *
 * Restart the card from scratch, as if from a cold-boot.
 */
static pci_ers_result_t wx_io_slot_reset(struct pci_dev *pdev)
{
	struct wx *wx = pci_get_drvdata(pdev);
	pci_ers_result_t result;

	if (pci_enable_device_mem(pdev)) {
		wx_err(wx, "Cannot re-enable PCI device after reset.\n");
		result = PCI_ERS_RESULT_DISCONNECT;
	} else {
		/* make all bar access done before reset. */
		smp_mb__before_atomic();
		clear_bit(WX_STATE_DISABLED, wx->state);
		pci_set_master(pdev);
		pci_restore_state(pdev);
		pci_save_state(pdev);
		pci_wake_from_d3(pdev, false);

		wx->do_reset(wx->netdev, false);
		result = PCI_ERS_RESULT_RECOVERED;
	}

	pci_aer_clear_nonfatal_status(pdev);

	return result;
}

/**
 * wx_io_resume - called when traffic can start flowing again.
 * @pdev: Pointer to PCI device
 *
 * This callback is called when the error recovery driver tells us that
 * its OK to resume normal operation.
 */
static void wx_io_resume(struct pci_dev *pdev)
{
	struct wx *wx = pci_get_drvdata(pdev);
	struct net_device *netdev;

	netdev = wx->netdev;
	rtnl_lock();
	if (netif_running(netdev))
		netdev->netdev_ops->ndo_open(netdev);

	netif_device_attach(netdev);
	rtnl_unlock();
}

const struct pci_error_handlers wx_err_handler = {
	.error_detected = wx_io_error_detected,
	.slot_reset = wx_io_slot_reset,
	.resume = wx_io_resume,
};
EXPORT_SYMBOL(wx_err_handler);

static void wx_reset_subtask(struct wx *wx)
{
	if (!test_bit(WX_FLAG_NEED_PF_RESET, wx->flags))
		return;

	if (!netif_running(wx->netdev) ||
	    test_bit(WX_STATE_RESETTING, wx->state))
		return;

	rtnl_lock();

	wx_warn(wx, "Reset adapter.\n");

	if (test_bit(WX_FLAG_NEED_PF_RESET, wx->flags)) {
		if (wx->do_reset)
			wx->do_reset(wx->netdev, true);
		clear_bit(WX_FLAG_NEED_PF_RESET,  wx->flags);
	}

	rtnl_unlock();
}

/*
 * wx_check_tx_hang_subtask - check for hung queues and dropped interrupts
 * @wx - pointer to the device wx structure
 *
 * This function serves two purposes.  First it strobes the interrupt lines
 * in order to make certain interrupts are occurring.  Secondly it sets the
 * bits needed to check for TX hangs.  As a result we should immediately
 * determine if a hang has occurred.
 */
static void wx_check_tx_hang_subtask(struct wx *wx)
{
	int i;

	/* If we're down or resetting, just bail */
	if (!netif_running(wx->netdev) ||
	    test_bit(WX_STATE_RESETTING, wx->state))
		return;

	/* Force detection of hung controller */
	if (netif_carrier_ok(wx->netdev)) {
		for (i = 0; i < wx->num_tx_queues; i++)
			set_bit(WX_TX_DETECT_HANG, wx->tx_ring[i]->state);
	}
}

void wx_handle_errors_subtask(struct wx *wx)
{
	wx_reset_subtask(wx);
	wx_check_tx_hang_subtask(wx);
}
EXPORT_SYMBOL(wx_handle_errors_subtask);

static void wx_tx_timeout_reset(struct wx *wx)
{
	if (!netif_running(wx->netdev))
		return;

	set_bit(WX_FLAG_NEED_PF_RESET, wx->flags);
	wx_warn(wx, "initiating reset due to tx timeout\n");
	wx_service_event_schedule(wx);
}

void wx_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	struct wx *wx = netdev_priv(netdev);
	u32 head, tail;
	int i;

	for (i = 0; i < wx->num_tx_queues; i++) {
		struct wx_ring *tx_ring = wx->tx_ring[i];

		if (test_bit(WX_TX_DETECT_HANG, tx_ring->state) &&
		    wx_check_tx_hang(tx_ring))
			wx_warn(wx, "Real tx hang detected on queue %d\n", i);

		head = rd32(wx, WX_PX_TR_RP(tx_ring->reg_idx));
		tail = rd32(wx, WX_PX_TR_WP(tx_ring->reg_idx));
		wx_warn(wx,
			"tx ring %d next_to_use is %d, next_to_clean is %d\n",
			i, tx_ring->next_to_use,
			tx_ring->next_to_clean);
		wx_warn(wx, "tx ring %d hw rp is 0x%x, wp is 0x%x\n",
			i, head, tail);
	}

	wx_tx_timeout_reset(wx);
}
EXPORT_SYMBOL(wx_tx_timeout);

void wx_handle_tx_hang(struct wx_ring *tx_ring, unsigned int next)
{
	struct wx *wx = netdev_priv(tx_ring->netdev);

	wx_warn(wx, "Detected Tx Unit Hang\n"
		"  Tx Queue             <%d>\n"
		"  TDH, TDT             <%x>, <%x>\n"
		"  next_to_use          <%x>\n"
		"  next_to_clean        <%x>\n"
		"tx_buffer_info[next_to_clean]\n"
		"  time_stamp           <%lx>\n"
		"  jiffies              <%lx>\n",
		tx_ring->queue_index,
		rd32(wx, WX_PX_TR_RP(tx_ring->reg_idx)),
		rd32(wx, WX_PX_TR_WP(tx_ring->reg_idx)),
		tx_ring->next_to_use, next,
		tx_ring->tx_buffer_info[next].time_stamp, jiffies);

	netif_stop_subqueue(tx_ring->netdev, tx_ring->queue_index);

	wx_warn(wx, "tx hang detected on queue %d, resetting adapter\n",
		tx_ring->queue_index);

	wx_tx_timeout_reset(wx);
}
