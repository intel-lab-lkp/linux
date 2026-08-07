// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015, 2016 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/bitmap.h>

#include "hfi2.h"
#include "file_ops.h"
#include "common.h"
#include "sdma.h"

#define LINK_UP_DELAY 500 /* in microseconds */

static void set_mgmt_allowed(struct hfi2_pportdata *ppd)
{
	u32 frame;
	struct hfi2_devdata *dd = ppd->dd;

	if (ppd->neighbor_type == NEIGHBOR_TYPE_HFI) {
		ppd->mgmt_allowed = 1;
	} else {
		hfi2_read_8051_config(dd, REMOTE_LNI_INFO, GENERAL_CONFIG,
				      &frame);
		ppd->mgmt_allowed = (frame >> MGMT_ALLOWED_SHIFT) &
				    MGMT_ALLOWED_MASK;
	}
}

/*
 * Our neighbor has indicated that we are allowed to act as a fabric
 * manager, so place the full management partition key in the second
 * (0-based) pkey array position. Note that we should already have
 * the limited management partition key in array element 1, and also
 * that the port is not yet up when add_full_mgmt_pkey() is invoked.
 */
static void add_full_mgmt_pkey(struct hfi2_pportdata *ppd)
{
	/* Sanity check - ppd->pkeys[2] should be 0, or already initialized */
	if (!((ppd->pkeys[2] == 0) || (ppd->pkeys[2] == FULL_MGMT_P_KEY)))
		ppd_dev_warn(
			ppd,
			"%s pkey[2] already set to 0x%x, resetting it to 0x%x\n",
			__func__, ppd->pkeys[2], FULL_MGMT_P_KEY);
	ppd->pkeys[2] = FULL_MGMT_P_KEY;
	(void)hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_PKEYS, 0);
	hfi2_event_pkey_change(ppd->dd, ppd->port);
}

static void signal_ib_event(struct hfi2_pportdata *ppd, enum ib_event_type ev)
{
	struct ib_event event;
	struct hfi2_devdata *dd = ppd->dd;

	/*
	 * Only call ib_dispatch_event() if the IB device has been
	 * registered.  HFI2_INITED is set iff the driver has successfully
	 * registered with the IB core.
	 */
	if (!(dd->flags & HFI2_INITTED))
		return;
	event.device = &dd->verbs_dev.rdi.ibdev;
	event.element.port_num = ppd->port;
	event.event = ev;
	ib_dispatch_event(&event);
}

/**
 * hfi2_handle_linkup_change - finish linkup/down state changes
 * @ppd: valid port data
 * @linkup: link state information
 *
 * Handle a linkup or link down notification.
 * The HW needs time to finish its link up state change. Give it that chance.
 *
 * This is called outside an interrupt.
 *
 */
void hfi2_handle_linkup_change(struct hfi2_pportdata *ppd, u32 linkup)
{
	struct hfi2_devdata *dd = ppd->dd;
	enum ib_event_type ev;

	if (dd->cport) {
		ppd_dev_err(ppd, "%s should not be called for JKR\n", __func__);
		return;
	}
	if (!(ppd->linkup ^ !!linkup))
		return; /* no change, nothing to do */

	if (linkup) {
		/*
		 * Quick linkup does not trigger or implement:
		 *	- VerifyCap interrupt
		 *	- VerifyCap frames
		 * But rather moves directly to LinkUp.
		 *
		 * Do the work of the VerifyCap interrupt handler,
		 * hfi2_handle_verify_cap(), but do not try moving the state to
		 * LinkUp as we are already there.
		 *
		 * NOTE: This uses this device's vAU, vCU, and vl15_init for
		 * the remote values.  Both sides must be using the values.
		 */
		if (hfi2_quick_linkup) {
			hfi2_set_up_vau(ppd, dd->vau);
			hfi2_set_up_vl15(ppd, dd->vl15_init);
			hfi2_assign_remote_cm_au_table(ppd, dd->vcu);
		}

		ppd->neighbor_guid =
			hfi2_read_csr(dd, DC_DC8051_STS_REMOTE_GUID);
		ppd->neighbor_type =
			hfi2_read_csr(dd, DC_DC8051_STS_REMOTE_NODE_TYPE) &
			DC_DC8051_STS_REMOTE_NODE_TYPE_VAL_MASK;
		ppd->neighbor_port_number =
			hfi2_read_csr(dd, DC_DC8051_STS_REMOTE_PORT_NO) &
			DC_DC8051_STS_REMOTE_PORT_NO_VAL_SMASK;
		ppd->neighbor_fm_security =
			hfi2_read_csr(dd, DC_DC8051_STS_REMOTE_FM_SECURITY) &
			DC_DC8051_STS_LOCAL_FM_SECURITY_DISABLED_MASK;
		ppd_dev_info(ppd, "Neighbor Guid %llx, Type %d, Port Num %d\n",
			     ppd->neighbor_guid, ppd->neighbor_type,
			     ppd->neighbor_port_number);

		/* HW needs LINK_UP_DELAY to settle, give it that chance */
		udelay(LINK_UP_DELAY);

		/*
		 * 'MgmtAllowed' information, which is exchanged during
		 * LNI, is available at this point.
		 */
		set_mgmt_allowed(ppd);

		if (ppd->mgmt_allowed)
			add_full_mgmt_pkey(ppd);

		/* physical link went up */
		ppd->linkup = 1;
		ppd->offline_disabled_reason =
			HFI2_ODR_MASK(OPA_LINKDOWN_REASON_NONE);

		/* link widths are not available until the link is fully up */
		hfi2_get_linkup_link_widths(ppd);

	} else {
		/* physical link went down */
		ppd->linkup = 0;

		/* clear HW details of the previous connection */
		ppd->actual_vls_operational = 0;
		hfi2_reset_link_credits(ppd);

		/* freeze after a link down to guarantee a clean egress */
		hfi2_start_freeze_handling(dd, FREEZE_SELF | FREEZE_LINK_DOWN);

		ev = IB_EVENT_PORT_ERR;

		hfi2_set_uevent_bits(ppd, _HFI2_EVENT_LINKDOWN_BIT);

		/* if we are down, the neighbor is down */
		ppd->neighbor_normal = 0;

		/* notify IB of the link change */
		signal_ib_event(ppd, ev);
	}
}

/* Special version of hfi2_handle_linkup_change() for systems with a CPORT */
void hfi2_cport_handle_linkup_change(struct hfi2_pportdata *ppd,
				     struct opa_port_info *pi, u32 linkup)
{
	struct hfi2_devdata *dd = ppd->dd;
	enum ib_event_type ev;

	if (!(ppd->linkup ^ !!linkup))
		return; /* no change, nothing to do */

	if (linkup) {
		ppd->neighbor_guid = be64_to_cpu(pi->neigh_node_guid);
		ppd->neighbor_port_number = pi->neigh_port_num;
		ppd->neighbor_type = pi->port_neigh_mode &
				     OPA_PI_MASK_NEIGH_NODE_TYPE;
		ppd->mgmt_allowed = !!(pi->port_neigh_mode &
				       OPA_PI_MASK_NEIGH_MGMT_ALLOWED);
		ppd->neighbor_fm_security = !!(
			pi->port_neigh_mode & OPA_PI_MASK_NEIGH_FW_AUTH_BYPASS);

		ppd_dev_info(ppd, "Neighbor Guid %llx, Type %d, Port Num %d\n",
			     ppd->neighbor_guid, ppd->neighbor_type,
			     ppd->neighbor_port_number);

		if (ppd->mgmt_allowed) {
			if (!(ppd->pkeys[2] == 0 ||
			      ppd->pkeys[2] == FULL_MGMT_P_KEY))
				ppd_dev_warn(
					ppd,
					"%s pkey[2] already set to 0x%x, resetting it to 0x%x\n",
					__func__, ppd->pkeys[2],
					FULL_MGMT_P_KEY);
			ppd->pkeys[2] = FULL_MGMT_P_KEY;
			hfi2_event_pkey_change(ppd->dd, ppd->port);
		}

		/* physical link went up */
		ppd->linkup = 1;
		ppd->offline_disabled_reason =
			HFI2_ODR_MASK(OPA_LINKDOWN_REASON_NONE);

		/* link widths are not available until the link is fully up */
		ppd->link_width_enabled = be16_to_cpu(pi->link_width.enabled);
		ppd->link_width_supported =
			be16_to_cpu(pi->link_width.supported);
		ppd->link_width_active = be16_to_cpu(pi->link_width.active);
		ppd->link_width_downgrade_supported =
			be16_to_cpu(pi->link_width_downgrade.supported);
		ppd->link_width_downgrade_enabled =
			be16_to_cpu(pi->link_width_downgrade.enabled);
		ppd->link_width_downgrade_tx_active =
			be16_to_cpu(pi->link_width_downgrade.tx_active);
		ppd->link_width_downgrade_rx_active =
			be16_to_cpu(pi->link_width_downgrade.rx_active);
		ppd->link_speed_supported =
			be16_to_cpu(pi->link_speed.supported);
		ppd->link_speed_active = be16_to_cpu(pi->link_speed.active);
		ppd->link_speed_enabled = be16_to_cpu(pi->link_speed.enabled);

		/*
		 * Rewrite the KDETH indicator.  The firmware overwrites it
		 * when resetting the link.  All ports are rewritten, but
		 * the same value is always used - a noop on other ports.
		 */
		hfi2_init_kdeth_qp(dd);

	} else {
		/* physical link went down */
		ppd->linkup = 0;

		/* clear HW details of the previous connection */
		ppd->actual_vls_operational = 0;

		/* what's left from hfi2_reset_link_credits() */
		dd->vl15buf_cached = 0;

		hfi2_start_linkdown_handling(ppd);

		ev = IB_EVENT_PORT_ERR;

		hfi2_set_uevent_bits(ppd, _HFI2_EVENT_LINKDOWN_BIT);

		/* if we are down, the neighbor is down */
		ppd->neighbor_normal = 0;

		/* notify IB of the link change */
		signal_ib_event(ppd, ev);
	}
}

/**
 * hfi2_go_port_active - All steps needed when the port goes active.
 * @ppd: port structure
 *
 * Take non-chip specific steps for transition from INIT to ACTIVE.  This
 * routine expects the port to already in INIT.  This routine is not
 * responsible for setting the state.
 */
void hfi2_go_port_active(struct hfi2_pportdata *ppd)
{
	signal_ib_event(ppd, IB_EVENT_PORT_ACTIVE);
}

/*
 * Handle receive or urgent interrupts for user contexts.  This means a user
 * process was waiting for a packet to arrive, and didn't want to poll.
 */
void hfi2_handle_user_interrupt(struct hfi2_ctxtdata *rcd)
{
	struct hfi2_devdata *dd = rcd->dd;
	unsigned long flags;

	spin_lock_irqsave(&dd->uctxt_lock, flags);
	if (bitmap_empty(rcd->in_use_ctxts, HFI2_MAX_SHARED_CTXTS))
		goto done;

	if (test_and_clear_bit(HFI2_CTXT_WAITING_RCV, &rcd->event_flags)) {
		wake_up_interruptible(&rcd->wait);
		hfi2_rcvctrl(dd, HFI2_RCVCTRL_INTRAVAIL_DIS, rcd);
	} else if (test_and_clear_bit(HFI2_CTXT_WAITING_URG,
				      &rcd->event_flags)) {
		rcd->urgent++;
		wake_up_interruptible(&rcd->wait);
	}
done:
	spin_unlock_irqrestore(&dd->uctxt_lock, flags);
}
