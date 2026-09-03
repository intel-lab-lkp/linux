// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * Generalized (parameterized) chip specific functions and variables.
 */

#include "hfi2.h"
#include "chip_gen.h"
#include "chip_jkr.h"
#include "cport_traps.h"
#include "vf2pf.h"
#include "sriov.h"

#undef DEBUG_CPORT_TRAP

#define SC(name) SEND_CTXT_##name

/*
 * Control the port LED state.  Cancel with hfi2_gen_shutdown_led_override().
 */
void hfi2_gen_setextled(struct hfi2_pportdata *ppd, u32 on)
{
	ppd_dev_warn(ppd, "%s: on %d, not implemented\n", __func__, on);
}

/*
 * Make the port LED blink in pattern.  Parameters timeon and timeoff are
 * in milliseconds.  Cancel with hfi2_gen_shutdown_led_override().
 */
void hfi2_gen_start_led_override(struct hfi2_pportdata *ppd, unsigned int timeon,
			    unsigned int timeoff)
{
	ppd_dev_warn(ppd, "%s: not implemented\n", __func__);

	/* used by the subnet manager to know if it set beaconing */
	atomic_set(&ppd->led_override_timer_active, 1);
	/* ensure the atomic_set is visible to all CPUs */
	smp_wmb();
}

/*
 * Return to normal LED operation.  This cancels overrides started with
 * hfi2_gen_setextled() or hfi2_gen_start_led_override().
 */
void hfi2_gen_shutdown_led_override(struct hfi2_pportdata *ppd)
{
	ppd_dev_warn(ppd, "%s: not implemented\n", __func__);

	/* used by the subnet manager to know if it set beaconing */
	atomic_set(&ppd->led_override_timer_active, 0);
	/* ensure the atomic_set is visible to all CPUs */
	smp_wmb();
}

void hfi2_gen_read_guid(struct hfi2_devdata *dd)
{
	dd_dev_warn(dd, "%s: not implemented\n", __func__);

	dd->base_guid = 0xabcd;
}

int hfi2_gen_late_per_chip_init(struct hfi2_devdata *dd)
{
	return 0;
}

void hfi2_gen_start_port(struct hfi2_pportdata *ppd)
{
	struct hfi2_devdata *dd = ppd->dd;
	u64 guid;

	/*
	 * Derive the port GUID from the device base GUID if not already set.
	 * Per OPA spec, bits 34:32 encode the port number (1-7).
	 */
	guid = ppd->guids[HFI2_PORT_GUID_INDEX];
	if (!guid) {
		/* OPA spec says bits 34:32 are port number, 1-7 */
		if (dd->base_guid)
			guid = (dd->base_guid & ~(7ULL << 32)) |
			       ((u64)ppd->port << 32);
		ppd->guids[HFI2_PORT_GUID_INDEX] = guid;
		pr_warn("%s: ppd->guids[HFI2_PORT_GUID_INDEX] = 0x%llx",
			__func__, guid);
	}
}

void hfi2_gen_stop_port(struct hfi2_pportdata *ppd)
{
	ppd_dev_warn(ppd, "%s: pidx %d, not implemented\n", __func__,
		     ppd->hw_pidx);
}

void hfi2_gen_set_port_max_mtu(struct hfi2_pportdata *ppd, u32 maxvlmtu)
{
	ppd_dev_warn(ppd, "%s: pidx %d, not implemented\n", __func__,
		     ppd->hw_pidx);
}

u64 hfi2_gen_create_pbc_pidx(u8 pidx, u64 flags, int srate_mbs, u32 vl, u32 dw_len,
			u32 l2, u32 dlid, u32 sctxt)
{
	/* always add ICRC for non 9B packets */
	if (l2 != PBC_L2_9B)
		flags |= PBC_INSERT_BYPASS_ICRC; /* AKA PbcInsertNon9bIcrc */

	return (u64)sctxt << PBC_SEND_CTXT_SHIFT | (u64)dlid << PBC_DLID_SHIFT |
	       /* lower 32 bits */
	       flags | PBC_IHCRC_NONE << PBC_INSERT_HCRC_SHIFT |
	       l2 << PBC_L2_TYPE_SHIFT | pidx << PBC_PORT_IDX_SHIFT |
	       (vl & PBC_VL_MASK) << PBC_VL_SHIFT |
	       (dw_len & PBC_LENGTH_DWS_MASK) << PBC_LENGTH_DWS_SHIFT;
}

/**
 * hfi2_gen_create_pbc - build a pbc for transmission
 * @ppd: info of physical Hfi port
 * @hfi2_loopback: whether to use hfi2_loopback port
 * @flags: special case flags or-ed in built pbc
 * @srate_mbs: static rate - unused
 * @vl: vl
 * @dw_len: dword length (header words + data words + pbc words)
 * @l2: L2 header field - determines type
 * @dlid: destination LID
 * @sctxt: send context number
 *
 * Create a PBC with the given flags, rate, VL, and length.
 *
 * NOTE: The PBC created will not insert any HCRC.
 */
u64 hfi2_gen_create_pbc(struct hfi2_pportdata *ppd, bool hfi2_loopback, u64 flags,
		   int srate_mbs, u32 vl, u32 dw_len, u32 l2, u32 dlid,
		   u32 sctxt)
{
	u8 pidx = hfi2_loopback ? loopback_pidx(ppd) : ppd->hw_pidx;

	return hfi2_gen_create_pbc_pidx(pidx, flags, srate_mbs, vl, dw_len, l2, dlid,
				   sctxt);
}

/*
 * Construct a OPA MAD for sending to CPORT.
 */
static struct opa_smp *build_cport_mad(int meth, int attr)
{
	struct opa_smp *mad;

	mad = kzalloc_obj(mad, GFP_KERNEL);
	if (!mad)
		return mad;
	mad->base_version = OPA_MGMT_BASE_VERSION;
	mad->mgmt_class = IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE;
	mad->class_version = OPA_SM_CLASS_VERSION;
	mad->method = meth;
	mad->attr_id = attr;
	return mad;
}

/*
 * Send a GET PORT_INFO OPA MAD to CPORT to get details on port.
 * Caller must kfree() the buffer returned (if not IS_ERR()).
 */
static struct opa_smp *cport_get_portinfo(struct hfi2_devdata *dd, int port)
{
	u8 sb = port; /* 1.. */
	struct opa_smp *mad;
	struct opa_smp *rsp;
	size_t rsp_len;
	int ret;

	mad = build_cport_mad(IB_MGMT_METHOD_GET, IB_SMP_ATTR_PORT_INFO);
	if (!mad)
		return ERR_PTR(-ENOMEM);
	/*
	 * Set port in attribute modifier field, for PORT_INFO.
	 */
	mad->attr_mod = cpu_to_be32(0x1000000 | port);
#ifdef DEBUG_CPORT_TRAP
	pr_warn("hfi2_%d: %s: send: %02x %02x %02x %02x - %04x %04x %08x\n",
		dd->unit, __func__, mad->base_version, mad->mgmt_class,
		mad->class_version, mad->method, be16_to_cpu(mad->status),
		be16_to_cpu(mad->attr_id), be32_to_cpu(mad->attr_mod));
#endif
	rsp_len = sizeof(*rsp);
	rsp = kzalloc(rsp_len, GFP_KERNEL);
	if (!rsp) {
		kfree(mad);
		return ERR_PTR(-ENOMEM);
	}
	ret = hfi2_cport_send_recv_mad(dd, sb, mad,
				  sizeof(*mad) - OPA_SMP_DR_DATA_SIZE, rsp,
				  &rsp_len);
	kfree(mad);
	if (ret) {
		kfree(rsp);
		if (ret > 0)
			ret = -EINVAL;
		return ERR_PTR(ret);
	}
#ifdef DEBUG_CPORT_TRAP
	pr_warn("hfi2_%d: %s: resp: %02x %02x %02x %02x - %04x %04x %08x\n",
		dd->unit, __func__, rsp->base_version, rsp->mgmt_class,
		rsp->class_version, rsp->method, be16_to_cpu(rsp->status),
		be16_to_cpu(rsp->attr_id), be32_to_cpu(rsp->attr_mod));
#endif
	return rsp;
}

/*
 * Called on PF0 after all new VFs appear.
 */
int hfi2_sriov_sync_ports(struct hfi2_devdata *dd, int si_mask)
{
	struct opa_smp *mad;
	struct opa_port_info *pi;
	int pidx;
	int ret = 0;

	if (dd->is_vf)
		return -EINVAL;
	for (pidx = 0; pidx < dd->params->num_ports; ++pidx) {
		mad = cport_get_portinfo(dd, pidx + 1);
		if (IS_ERR(mad)) {
			ret = PTR_ERR(mad);
		} else {
			pi = (struct opa_port_info *)opa_get_smp_data(mad);
			ret = hfi2_pf2vf_push_portinfo(&dd->pport[pidx], mad, pi,
						  si_mask);
			kfree(mad);
		}
		if (!ret)
			ret = hfi2_pf2vf_push_sc2vlt(&dd->pport[pidx], si_mask);
	}
	return ret;
}

#ifdef DEBUG_CPORT_TRAP
static const char *ps_state_name(struct opa_port_states *ps)
{
	static const char *const state_name[] = {
		[IB_PORT_NOP] = "NOP",
		[IB_PORT_DOWN] = "DOWN",
		[IB_PORT_INIT] = "INIT",
		[IB_PORT_ARMED] = "ARMED",
		[IB_PORT_ACTIVE] = "ACTIVE",
		[IB_PORT_ACTIVE_DEFER] = "ACTIVE_DEFER"
	};
	u8 ls = port_states_to_logical_state(ps);

	if (ls > IB_PORT_ACTIVE_DEFER)
		return "???";
	return state_name[ls];
}
#endif

static void check_cport_state(struct work_struct *work)
{
	struct hfi2_cport *cport =
		container_of(work, struct hfi2_cport, psc.work);
	struct hfi2_devdata *dd = cport->dd;
	struct opa_smp *mad;
	struct opa_port_info *pi;
	int ret;
	int pidx;

	/*
	 * There should be only one running. Others could abort except for
	 * the race between checking states and releasing semaphore.
	 */
	ret = down_killable(&dd->cport->psc.wait);
	if (ret) {
		atomic_dec(&dd->cport->psc.nq);
		return;
	}
#ifdef DEBUG_CPORT_TRAP
	pr_warn("hfi2_%d: %s: starting port_info loop\n", dd->unit, __func__);
#endif

	for (pidx = 0; pidx < dd->params->num_ports; ++pidx) {
		if (!port_available_pidx(dd, pidx)) {
			ppd_dev_info(
				&dd->pport[pidx],
				"Skipping port state check - port not available\n");
			continue;
		}
		mad = cport_get_portinfo(dd, pidx + 1);
		if (IS_ERR(mad)) {
			ret = PTR_ERR(mad);
		} else {
			pi = (struct opa_port_info *)opa_get_smp_data(mad);
#ifdef DEBUG_CPORT_TRAP
			pr_warn("hfi2_%d: %s: PORTINFO %d: %s %08x (%x)\n",
				dd->unit, __func__, pidx + 1,
				ps_state_name(&pi->port_states),
				be32_to_cpu(mad->attr_mod),
				be16_to_cpu(mad->status));
#endif
			ret = hfi2_update_from_opa_portinfo(&dd->pport[pidx], mad,
						       pi);
			kfree(mad);
		}
		if (ret)
			dd_dev_warn(
				dd,
				"Failed to update PORT_INFO on port %d (%d)\n",
				pidx + 1, ret);
	}
#ifdef DEBUG_CPORT_TRAP
	pr_warn("hfi2_%d: %s: finished port_info loop\n", dd->unit, __func__);
#endif
	atomic_dec(&dd->cport->psc.nq);
	up(&dd->cport->psc.wait);
}

static void handle_cport_trap128(struct hfi2_devdata *dd,
				 struct cport_trap_status traps)
{
	/* note: traps are already repressed */
#ifdef DEBUG_CPORT_TRAP
	pr_warn("hfi2_%d: %s: TRAP128 psc=%d\n", dd->unit, __func__,
		!!(traps.flags & CPORT_TRAP_PSC));
#endif

	if (atomic_read(&dd->cport->psc.nq) > 1) {
#ifdef DEBUG_CPORT_TRAP
		pr_warn("hfi2_%d: %s: TRAP128(s) pending: %d\n", dd->unit,
			__func__, atomic_read(&dd->cport->psc.nq));
#endif
		return;
	}
	atomic_inc(&dd->cport->psc.nq);
	queue_work(dd->hfi2_wq, &dd->cport->psc.work);
}

/*
 * This initializes everything necessary to receive and process Port
 * State Change TRAPs from CPORT. It also kicks off the initial gathering
 * of port states from CPORT.
 */
int hfi2_init_cport_trap128(struct hfi2_devdata *dd)
{
	struct cport_trap_status traps = { 0 };
	int ret = 0;

	if (!dd->cport)
		return 0;

	atomic_set(&dd->cport->psc.nq, 0);
	sema_init(&dd->cport->psc.wait, 1);
	INIT_WORK(&dd->cport->psc.work, check_cport_state);
	traps.flags = CPORT_TRAP_PSC; /* Trap 128 Port State Change */
	ret = hfi2_register_cport_trap(dd, traps, handle_cport_trap128);
	if (ret)
		dd_dev_warn(dd, "Failed to register for CPORT TRAP 128: %d\n",
			    ret);
	else if (!(dd->cport->traps_act.flags & CPORT_TRAP_PSC))
		dd_dev_warn(dd, "CPORT TRAP128 not supported\n");
	/* Fake a TRAP-128 to gather initial port states even if register fails */
	handle_cport_trap128(dd, traps);
	return ret;
}

int hfi2_deinit_cport_trap128(struct hfi2_devdata *dd)
{
	if (!dd->cport || !(dd->cport->traps.flags & CPORT_TRAP_PSC))
		return 0;
	return hfi2_deregister_cport_trap(dd, handle_cport_trap128);
}

static void handle_cport_overtemp(struct hfi2_devdata *dd,
				  struct cport_trap_status traps)
{
	/* note: traps are already repressed */
	hfi2_overtemp(dd);
}

/* no deinit_ - clearall_cport_trap() unregisters this */
int hfi2_init_cport_overtemp(struct hfi2_devdata *dd)
{
	struct cport_trap_status traps = { 0 };
	int ret = 0;

	if (!dd->cport)
		return 0;

	traps.flags = CPORT_TRAP_OVTM; /* Over Temp emergency */
	ret = hfi2_register_cport_trap(dd, traps, handle_cport_overtemp);
	if (ret)
		dd_dev_warn(dd, "Failed to register for CPORT Over Temp: %d\n",
			    ret);
	else if (!(dd->cport->traps_act.flags & CPORT_TRAP_OVTM))
		dd_dev_warn(dd, "CPORT Over-Temp notification not supported\n");
	return ret;
}

static int cport_goto_offline(struct hfi2_pportdata *ppd,
			      struct opa_port_info *pi, u8 rem_reason)
{
	u32 previous_state;

	previous_state = ppd->host_link_state;
	ppd->host_link_state = HLS_GOING_OFFLINE;

	/* start offline transition */
	if (ppd->offline_disabled_reason ==
	    HFI2_ODR_MASK(OPA_LINKDOWN_REASON_NONE))
		ppd->offline_disabled_reason =
			HFI2_ODR_MASK(OPA_LINKDOWN_REASON_TRANSIENT);

	hfi2_update_statusp(ppd, IB_PORT_DOWN);

	/*
	 * The state in CPORT is now offline.
	 *	- change our state
	 *	- notify others if we were previously in a linkup state
	 */
	ppd->host_link_state = HLS_DN_OFFLINE;
	if (previous_state & HLS_UP) {
		/* went down while link was up */
		hfi2_cport_handle_linkup_change(ppd, pi, 0);
	}

	/* the active link width (downgrade) is 0 on link down */
	ppd->link_width_active = 0;
	ppd->link_width_downgrade_tx_active = 0;
	ppd->link_width_downgrade_rx_active = 0;
	ppd->current_egress_rate = 0;
	return 0;
}

/* hfi2_set_link_state() for CPORT-based systems. Only update local data. */
int hfi2_cport_set_link_state(struct hfi2_pportdata *ppd, struct opa_port_info *pi,
			 u32 state)
{
	struct hfi2_devdata *dd = ppd->dd;
	int ret = 0;
	int orig_new_state, poll_bounce;

	mutex_lock(&ppd->hls_lock);

	orig_new_state = state;
	if (state == HLS_DN_DOWNDEF)
		state = HLS_DEFAULT;

	/* interpret poll -> poll as a link bounce */
	poll_bounce = ppd->host_link_state == HLS_DN_POLL &&
		      state == HLS_DN_POLL;

	ppd_dev_info(ppd, "%s: current %s, new %s %s%s\n", __func__,
		     hfi2_link_state_name(ppd->host_link_state),
		     hfi2_link_state_name(orig_new_state),
		     poll_bounce ? "(bounce) " : "",
		     hfi2_link_state_reason_name(ppd, state));

	/*
	 * If we're going to a (HLS_*) link state that implies the logical
	 * link state is neither of (IB_PORT_ARMED, IB_PORT_ACTIVE), then
	 * reset is_sm_config_started to 0.
	 */
	if (!(state & (HLS_UP_ARMED | HLS_UP_ACTIVE)))
		ppd->is_sm_config_started = 0;

	/*
	 * Do nothing if the states match.  Let a poll to poll link bounce
	 * go through.
	 */
	if (ppd->host_link_state == state && !poll_bounce)
		goto done;

	switch (state) {
	case HLS_UP_INIT:
		hfi2_log_state_transition(ppd, PLS_LINKUP);

		/* clear old transient LINKINIT_REASON code */
		if (ppd->linkinit_reason >= OPA_LINKINIT_REASON_CLEAR)
			ppd->linkinit_reason = OPA_LINKINIT_REASON_LINKUP;

		hfi2_cport_handle_linkup_change(ppd, pi, 1);
		hfi2_pio_kernel_linkup(ppd);

		/*
		 * After link up, a new link width will have been set.
		 * Update the xmit counters with regards to the new
		 * link width.
		 */
		hfi2_update_xmit_counters(ppd, ppd->link_width_active);

		ppd->host_link_state = HLS_UP_INIT;
		hfi2_update_statusp(ppd, IB_PORT_INIT);
		break;
	case HLS_UP_ARMED:
		if (ppd->host_link_state != HLS_UP_INIT)
			dd_dev_err(
				dd,
				"%s %d: allowing unexpected state transition from %s to %s\n",
				__func__, ppd->port,
				hfi2_link_state_name(ppd->host_link_state),
				hfi2_link_state_name(state));

		ppd->host_link_state = HLS_UP_ARMED;
		hfi2_update_statusp(ppd, IB_PORT_ARMED);
		break;
	case HLS_UP_ACTIVE:
		if (ppd->host_link_state != HLS_UP_ARMED)
			dd_dev_err(
				dd,
				"%s %d: allowing unexpected state transition from %s to %s\n",
				__func__, ppd->port,
				hfi2_link_state_name(ppd->host_link_state),
				hfi2_link_state_name(state));

		ppd->host_link_state = HLS_UP_ACTIVE;
		hfi2_update_statusp(ppd, IB_PORT_ACTIVE);
		hfi2_go_port_active(ppd);
		break;
	case HLS_DN_POLL:

		if (ppd->host_link_state != HLS_DN_OFFLINE) {
			u8 tmp = ppd->link_enabled;

			ret = cport_goto_offline(ppd, pi,
						 ppd->remote_link_down_reason);
			if (ret) {
				ppd->link_enabled = tmp;
				break;
			}
			ppd->remote_link_down_reason = 0;

			if (ppd->driver_link_ready)
				ppd->link_enabled = 1;
		}

		hfi2_set_all_slowpath(ppd);

		ppd->port_error_action = 0;

		ppd->host_link_state = HLS_DN_POLL;
		ppd->offline_disabled_reason =
			HFI2_ODR_MASK(OPA_LINKDOWN_REASON_NONE);
		hfi2_log_state_transition(ppd, PLS_POLLING);
		break;
	case HLS_DN_DISABLE:
		/* link is disabled */
		ppd->link_enabled = 0;

		/* allow any state to transition to disabled */

		/* must transition to offline first */
		if (ppd->host_link_state != HLS_DN_OFFLINE) {
			ret = cport_goto_offline(ppd, pi,
						 ppd->remote_link_down_reason);
			if (ret)
				break;
			ppd->remote_link_down_reason = 0;
		}

		ppd->host_link_state = HLS_DN_DISABLE;
		break;
	case HLS_DN_OFFLINE:
		/* allow any state to transition to offline */
		ret = cport_goto_offline(ppd, pi, ppd->remote_link_down_reason);
		if (!ret)
			ppd->remote_link_down_reason = 0;
		break;
	case HLS_GOING_UP: /* never seen by driver */
	case HLS_VERIFY_CAP: /* never seen by driver */
	case HLS_GOING_OFFLINE: /* transient within goto_offline() */
	case HLS_LINK_COOLDOWN: /* transient within goto_offline() */
	default:
		dd_dev_info(dd, "%s %d: state 0x%x: not supported\n", __func__,
			    ppd->port, state);
		ret = -EINVAL;
		break;
	}

done:
	mutex_unlock(&ppd->hls_lock);

	return ret;
}

int hfi2_cport_start_link(struct hfi2_pportdata *ppd, struct opa_port_info *pi)
{
	/*
	 * FULL_MGMT_P_KEY is cleared from the pkey table, so that the
	 * pkey table can be configured properly if the HFI unit is connected
	 * to switch port with MgmtAllowed=NO
	 */
	/* this writes CSRs... clear_full_mgmt_pkey(ppd); so do: */
	if (ppd->pkeys[2] != 0) {
		ppd->pkeys[2] = 0;
		/* avoid hfi2_set_ib_cfg(HFI2_IB_CFG_PKEYS) */
		hfi2_event_pkey_change(ppd->dd, ppd->port);
	}

	return hfi2_cport_set_link_state(ppd, pi, HLS_DN_POLL);
}

/**
 * hfi2_cport_read_temp - Ask cport firmware for the temperature.
 * @dd: device data
 * @gen_temp: temperature output.
 *
 * Return: 0 on success, -EINVAL on invalid reply from CPORT,
 * -EOPNOTSUPP on reply from CPORT but ASIC temperature not
 * valid/supported.
 */
int hfi2_cport_read_temp(struct hfi2_devdata *dd, struct cport_temp *gen_temp)
{
	struct cport_how_payload *how = NULL;
	int resp_len = 0;
	int ret;

	/* Don't trust the caller; assume invalid */
	gen_temp->asic_valid = 0;
	gen_temp->qsfp1_valid = 0;
	gen_temp->qsfp2_valid = 0;

	ret = hfi2_cport_send_req(dd, CH_OP_HOW, 0, NULL, 0, (void **)&how,
			     &resp_len, hfi2_cport_adm_to * HZ);
	if (ret) {
		dd_dev_err(dd, "CPORT how failed %d\n", ret);
		goto done;
	}
	if (resp_len != sizeof(*how)) {
		dd_dev_err(
			dd,
			"CPORT how invalid response length %d (expected %ld)\n",
			resp_len, sizeof(*how));
		ret = -EINVAL;
		goto done;
	}
	if (!CPORT_HOW_TEMP_VALID(how->temps)) {
		ret = -EOPNOTSUPP;
		goto done;
	}
	gen_temp->asic_valid = 1;
	gen_temp->asic = (s16)CPORT_HOW_TEMP(how->temps);

	gen_temp->qsfp1_valid = CPORT_HOW_QSFP1_TEMP_VALID(how->temps);
	if (gen_temp->qsfp1_valid)
		gen_temp->qsfp1 = (s16)CPORT_HOW_QSFP1_TEMP(how->temps);

	gen_temp->qsfp2_valid = CPORT_HOW_QSFP2_TEMP_VALID(how->temps);
	if (gen_temp->qsfp2_valid)
		gen_temp->qsfp2 = (s16)CPORT_HOW_QSFP2_TEMP(how->temps);
done:
	kfree(how);
	return ret;
}

static void gen_reset_rcvarray(struct hfi2_devdata *dd, u16 ctxt, u32 ra_cnt)
{
	u8 __iomem *ra;
	u32 off;
	u32 idx;

	ra = dd->bar_maps[ctxt_bar_idx(ctxt)].rcvarray_wc;
	ctxt = ctxt_bar_ctxt(ctxt);
	for (idx = 0; idx < ra_cnt; ++idx) {
		off = (ctxt << JKR_RCV_ARRAY_RCV_CTXT_IDX_SHIFT) |
		      (idx << JKR_RCV_ARRAY_CSR_INDEX_SHIFT);
		writeq(RCV_ARRAY_RT_WRITE_ENABLE_SMASK, ra + off);
	}
	flush_wc();
}

/*
 * Called on PF0 before VFs are created.
 * Context will be used for Eager only (no TID).
 * Initialize all CSRs that can only be accessed by PF0.
 * May be called to reset context for re-use.
 */
int hfi2_gen_init_rctxt_egr(struct hfi2_devdata *dd, u8 pidx, int si, u16 ctxt,
		       u32 ra_base, u32 ra_cnt, u32 hdr_size)
{
	u64 reg, kreg;

	/* might need to reclaim context in PF0 */
	if (si)
		write_rctxt_csr(dd, ctxt, JKR_RCV_SI_IDX, 0);

	/* reset eager head/tail by enabling ctxt after write of 0 to heads */
	kreg = read_kctxt_csr(dd, ctxt, dd->params->rcv_kctxt_ctrl_reg);

	/* disable context, in case it was previously used */
	hfi2_jkr_ena_rcv_ctxt(dd, pidx, ctxt, false);
	/* remove RCV_CTXT_CTRL_ENABLE_SMASK (disable) */
	kreg &= ~RCV_CTXT_CTRL_ENABLE_SMASK;
	/* force these bits */
	kreg |= RCV_CTXT_CTRL_ONE_PACKET_PER_EGR_BUFFER_SMASK |
		JKR_RCV_KCTXT_CTRL_RECEIVE_CUT_THROUGH_DISABLE_SMASK;
	write_kctxt_csr(dd, ctxt, dd->params->rcv_kctxt_ctrl_reg, kreg);

	dd->params->set_port_tid_config(dd, pidx, ctxt, ra_base, ra_cnt, 0, 0);
	hfi2_jkr_upd_rcv_hdr_size(dd, pidx, ctxt, hdr_size);

	reg = RCV_CTXT_CTRL_INTR_AVAIL_SMASK;
	write_rctxt_csr(dd, ctxt, dd->params->rcv_rctxt_ctrl_reg, reg);

	write_uctxt_csr(dd, ctxt, dd->params->rcv_hdr_head_reg, 0);
	write_uctxt_csr(dd, ctxt, dd->params->rcv_egr_index_head_reg, 0);

	gen_reset_rcvarray(dd, ctxt, ra_cnt);

	/* (re-)enable context */
	hfi2_jkr_ena_rcv_ctxt(dd, pidx, ctxt, true);
	kreg |= RCV_CTXT_CTRL_ENABLE_SMASK;
	write_kctxt_csr(dd, ctxt, dd->params->rcv_kctxt_ctrl_reg, kreg);

	/* must be done after enable */
	write_kctxt_csr(dd, ctxt, dd->params->rcv_avail_time_out_reg,
			RCV_AVAIL_TIME_OUT_TIME_OUT_RELOAD_MASK
				<< RCV_AVAIL_TIME_OUT_TIME_OUT_RELOAD_SHIFT);
	hfi2_update_usrhead_ctxt(dd, ctxt, 0, 1, 0, 0); /* needed for interrupts */

	/*
	 * Leave something for the VF to probe on.
	 * Set any non-zero value, will be changed by VF later.
	 */
	reg = ((u64)hfi2_encode_rcv_header_entry_size(32) &
	       RCV_HDR_ENT_SIZE_ENT_SIZE_MASK)
	      << RCV_HDR_ENT_SIZE_ENT_SIZE_SHIFT;
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_ent_size_reg, reg);

	/* finally, assign context to VF */
	if (si)
		write_rctxt_csr(dd, ctxt, JKR_RCV_SI_IDX, si);
	return 0;
}

void hfi2_gen_deinit_rctxt(struct hfi2_devdata *dd, u8 pidx, int si, u16 ctxt)
{
	u32 ra_cnt;

	/* first, assign context back to PF0 */
	if (si)
		write_rctxt_csr(dd, ctxt, JKR_RCV_SI_IDX, 0);

	ra_cnt = ((read_rctxt_csr(dd, ctxt, dd->params->rcv_egr_ctrl_reg) >>
		   RCV_EGR_CTRL_EGR_CNT_SHIFT) &
		  RCV_EGR_CTRL_EGR_CNT_MASK)
		 << RCV_SHIFT;
	hfi2_jkr_ena_rcv_ctxt(dd, pidx, ctxt, false);

	gen_reset_rcvarray(dd, ctxt, ra_cnt);

	write_kctxt_csr(dd, ctxt, dd->params->rcv_kctxt_ctrl_reg, 0);
	write_rctxt_csr(dd, ctxt, dd->params->rcv_rctxt_ctrl_reg, 0);
	write_rctxt_csr(dd, ctxt, dd->params->rcv_egr_ctrl_reg, 0);

	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_cnt_reg, 0);
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_ent_size_reg, 0);
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_addr_reg, 0);
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_tail_addr_reg, 0);
}

/*
 * Called by VFs before first VF-PF message.
 */
int hfi2_gen_start_rctxt_egr(struct hfi2_devdata *dd, u8 pidx, u16 ctxt,
			struct hfi2_ctxtbufs *bufs)
{
	u8 __iomem *ra;
	u16 order;
	u32 off;
	u64 reg;
	u32 r_each, r_size, etail;
	dma_addr_t r_dma;
	int idx;

	/* cleanup from anything sent while no driver */
	etail = read_uctxt_csr(dd, ctxt, JKR_RCV_EGR_INDEX_TAIL) & 0xffff;
	if (etail)
		hfi2_update_usrhead_ctxt(dd, ctxt, 0, 1, 1, etail);
	/* just clear overflow coount - can't do anything else */
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_ovfl_cnt_reg, 0);

	/* assumes RCV_CTXT_CTRL_ONE_PACKET_PER_EGR_BUFFER_SMASK is set */
	r_each = bufs->egr_buf_size;
	r_dma = bufs->egr.dma;
	r_size = bufs->egr.size;
	idx = 0;
	order = hfi2_encoded_size(r_each);
	ra = dd->bar_maps[ctxt_bar_idx(ctxt)].rcvarray_wc;
	while (r_size >= r_each) {
		off = (ctxt_bar_ctxt(ctxt)
		       << JKR_RCV_ARRAY_RCV_CTXT_IDX_SHIFT) |
		      (idx << JKR_RCV_ARRAY_CSR_INDEX_SHIFT);
		reg = RCV_ARRAY_RT_WRITE_ENABLE_SMASK |
		      ((u64)order << JKR_RCV_ARRAY_EGR_RT_BUF_SIZE_SHIFT) |
		      (r_dma >> RT_ADDR_SHIFT);
		writeq(reg, ra + off);
		++idx;
		r_size -= r_each;
		r_dma += r_each;
	}
	flush_wc();
	if (!idx) /* none allocated */
		return -ENOSPC;

	reg = (((u64)bufs->rhq_cnt >> HDRQ_SIZE_SHIFT) & RCV_HDR_CNT_CNT_MASK)
	      << RCV_HDR_CNT_CNT_SHIFT;
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_cnt_reg, reg);
	reg = ((u64)hfi2_encode_rcv_header_entry_size(bufs->rhq_ent_size) &
	       RCV_HDR_ENT_SIZE_ENT_SIZE_MASK)
	      << RCV_HDR_ENT_SIZE_ENT_SIZE_SHIFT;
	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_ent_size_reg, reg);

	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_addr_reg, bufs->rhq.dma);
	if (dd->params->set_rheq_addr)
		dd->params->set_rheq_addr(dd, ctxt, bufs->rheq.dma);

	write_kctxt_csr(dd, ctxt, dd->params->rcv_hdr_tail_addr_reg,
			dd->rcvhdrtail_dummy_dma);

	return 0;
}

/*
 * Called on PF0 before VFs are created.
 * Context is used for PIO only (no SDMA).
 * Initialize all CSRs that can only be accessed by PF0.
 */
int hfi2_gen_init_sctxt_pio(struct hfi2_devdata *dd, u8 pidx, int si, u16 ctxt,
		       u32 cr_base, u32 cr_cnt)
{
	u64 reg;
	int ret;

	/* might need to reclaim context in PF0 */
	if (si)
		hfi2_write_csr(dd, JKR_SEND_CTXT_SI_IDX + (8 * ctxt), 0);

	/* first, ensure context is disabled - to ensure reset */
	ret = hfi2_priv_reg_op(dd, pidx, ctxt, SC_KERNEL, SC_DISABLE_OP, 0);
	if (ret)
		return ret;

	reg = ((u64)cr_cnt << SEND_CTXT_CTRL_CTXT_DEPTH_SHIFT) |
	      ((u64)cr_base << dd->params->pio_base_shift);
	write_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg, reg);
	/* or: dd->params->set_pio_integrity(dd, pidx, ctxt, SC_KERNEL, SPI_INIT) */
	write_epsc_csr(dd, pidx, ctxt, dd->params->send_ctxt_check_enable_reg,
		       JKR_SEND_CTXT_CHECK_ENABLE_L2_TYPE9BALLOWED_SMASK);
	write_epsc_csr(dd, pidx, ctxt,
		       dd->params->send_ctxt_check_partition_key_reg,
		       (SEND_CTXT_CHECK_PARTITION_KEY_VALUE_MASK & DEFAULT_PKEY)
			       << SEND_CTXT_CHECK_PARTITION_KEY_VALUE_SHIFT);
	write_epsc_csr(dd, pidx, ctxt, dd->params->send_ctxt_check_opcode_reg,
		       ((u64)OPCODE_CHECK_MASK_DISABLED
			<< SEND_CTXT_CHECK_OPCODE_MASK_SHIFT) |
			       ((u64)OPCODE_CHECK_VAL_DISABLED
				<< SEND_CTXT_CHECK_OPCODE_VALUE_SHIFT));

	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_err_mask_reg, 0);
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_credit_return_addr_reg,
			0);

	reg = 1 << SEND_CTXT_CREDIT_CTRL_THRESHOLD_SHIFT;
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_credit_ctrl_reg, reg);
	/* send_ctxt_check_vl_reg stays 0? */

	/* this does a PIO init on the context */
	ret = hfi2_priv_reg_op(dd, pidx, ctxt, SC_KERNEL, SC_ENABLE_OP, 0);
	if (ret)
		return ret;

	/* finally, assign context to VF */
	if (si)
		hfi2_write_csr(dd, JKR_SEND_CTXT_SI_IDX + (8 * ctxt), si);

	return 0;
}

void hfi2_gen_deinit_sctxt(struct hfi2_devdata *dd, u8 pidx, int si, u16 ctxt)
{
	/* first, assign context back to PF0 */
	if (si)
		hfi2_write_csr(dd, JKR_SEND_CTXT_SI_IDX + (8 * ctxt), 0);

	write_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg, 0);
	write_epsc_csr(dd, pidx, ctxt, dd->params->send_ctxt_check_enable_reg,
		       0);
	write_epsc_csr(dd, pidx, ctxt,
		       dd->params->send_ctxt_check_partition_key_reg, 0);
	write_epsc_csr(dd, pidx, ctxt, dd->params->send_ctxt_check_opcode_reg,
		       0);
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_err_mask_reg, 0);
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_credit_return_addr_reg,
			0);
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_credit_ctrl_reg, 0);
}

/*
 * Called by VFs before first VF-PF message.
 */
int hfi2_gen_start_sctxt(struct hfi2_devdata *dd, u8 pidx, u16 ctxt,
		    struct hfi2_ctxtbufs *bufs)
{
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_err_mask_reg, (u64)-1);
	write_sctxt_csr(dd, ctxt, dd->params->send_ctxt_credit_return_addr_reg,
			bufs->cr.dma);
	return 0;
}

static void set_sc_check(struct hfi2_devdata *dd, u8 pidx, u32 ctxt, int type)
{
	u8 opval, opmask;

	/* set the default partition key */
	write_epsc_csr(dd, pidx, ctxt,
		       dd->params->send_ctxt_check_partition_key_reg,
		       (SC(CHECK_PARTITION_KEY_VALUE_MASK) & DEFAULT_PKEY)
			       << SC(CHECK_PARTITION_KEY_VALUE_SHIFT));
	/* per context type checks */
	if (type == SC_USER) {
		opval = USER_OPCODE_CHECK_VAL;
		opmask = USER_OPCODE_CHECK_MASK;
	} else {
		opval = OPCODE_CHECK_VAL_DISABLED;
		opmask = OPCODE_CHECK_MASK_DISABLED;
	}
	/* set the send context check opcode mask and value */
	write_epsc_csr(dd, pidx, ctxt, dd->params->send_ctxt_check_opcode_reg,
		       ((u64)opmask << SC(CHECK_OPCODE_MASK_SHIFT)) |
			       ((u64)opval << SC(CHECK_OPCODE_VALUE_SHIFT)));
	/* User send contexts should not allow sending on VL15 */
	if (type == SC_USER) {
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_vl_reg, 1ULL << 15);
	}
}

/*
 * Read a CSR based on type
 *
 * type - CSR_TYPE_*
 * off - base offset of CSR
 * ctxt - conext number, if type requires one
 * pidx_eng - port index or SDMA engine number, depending on type
 */
u64 hfi2_read_csr_type(struct hfi2_devdata *dd, enum csr_type type, u32 off,
		  u16 ctxt, u8 pidx_eng)
{
	u64 reg = ~0ull;

	switch (type) {
	case CSR_TYPE_IPORT:
		reg = read_iport_csr(dd, pidx_eng, off);
		break;
	case CSR_TYPE_IPRC:
		reg = read_iprc_csr(dd, pidx_eng, ctxt, off);
		break;
	case CSR_TYPE_RCTXT:
		reg = read_rctxt_csr(dd, ctxt, off);
		break;
	case CSR_TYPE_KCTXT:
		reg = read_kctxt_csr(dd, ctxt, off);
		break;
	case CSR_TYPE_KU:
		reg = read_ku_csr(dd, ctxt, off);
		break;
	case CSR_TYPE_UCTXT:
		reg = read_uctxt_csr(dd, ctxt, off);
		break;
	case CSR_TYPE_SCTXT:
		reg = read_sctxt_csr(dd, ctxt, off);
		break;
	case CSR_TYPE_TCTXT:
		reg = read_tctxt_csr(dd, ctxt, off);
		break;
	case CSR_TYPE_SDMA:
		reg = read_sdma_csr(dd, pidx_eng, off);
		break;
	case CSR_TYPE_SDMACFG:
		reg = read_sdmacfg_csr(dd, pidx_eng, off);
		break;
	case CSR_TYPE_EPORT:
		reg = read_eport_csr(dd, pidx_eng, off);
		break;
	case CSR_TYPE_EPSC:
		reg = read_epsc_csr(dd, pidx_eng, ctxt, off);
		break;
	case CSR_TYPE_EPSCARR:
		reg = read_epsc_csr(dd, pidx_eng, ctxt, off);
		break;
	}
	return reg;
}

int hfi2_priv_reg_op(struct hfi2_devdata *dd, int pidx, u32 ctxt, int type,
		enum preg_op op, u64 arg)
{
	u16 rctxt;
	int ret = 0;

	rctxt = ctxt >> 16;
	ctxt &= 0xffff;

	if (dd->is_vf) {
		ret = hfi2_vf2pf_priv_reg_op(dd, pidx, ctxt, type, op, arg);
		if (ret)
			dd_dev_err(dd, "hfi2_vf2pf_priv_reg_op(%d) failed %d\n", op,
				   ret);
		return ret;
	}

	/* Only PF0 has access to these CSRs */
	switch (op) {
	case SC_CHK_ALLOC_OP: /* 'arg' is send_ctxt_ctrl_reg value */
		write_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg, arg);
		dd->params->set_pio_integrity(dd, pidx, ctxt, type,
					      SPI_DEFAULT);
		set_sc_check(dd, pidx, ctxt, type);
		if (dd->is_sriov)
			set_sc_check(dd, loopback_pidx_dd(dd, pidx), ctxt,
				     type);
		break;
	case SC_CHK_FREE_OP: /* 'arg' not used */
		write_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg, 0);
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_enable_reg, 0);
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_partition_key_reg,
			       0);
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_opcode_reg, 0);
		if (dd->is_sriov) {
			pidx = loopback_pidx_dd(dd, pidx);
			write_epsc_csr(dd, pidx, ctxt,
				       dd->params->send_ctxt_check_enable_reg,
				       0);
			write_epsc_csr(
				dd, pidx, ctxt,
				dd->params->send_ctxt_check_partition_key_reg,
				0);
			write_epsc_csr(dd, pidx, ctxt,
				       dd->params->send_ctxt_check_opcode_reg,
				       0);
		}
		break;
	case SC_CHK_VL_MASK_OP: /* 'arg' is send_ctxt_check_vl_reg value */
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_vl_reg, arg);
		if (dd->is_sriov)
			write_epsc_csr(dd, loopback_pidx_dd(dd, pidx), ctxt,
				       dd->params->send_ctxt_check_vl_reg, arg);
		break;
	case SC_CHK_SLID_OP: /* 'arg' is send_ctxt_check_slid_reg value */
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_slid_reg, arg);
		if (dd->is_sriov)
			write_epsc_csr(dd, loopback_pidx_dd(dd, pidx), ctxt,
				       dd->params->send_ctxt_check_slid_reg,
				       arg);
		break;
	case SC_CHK_JKEY_OP: /* 'arg' is send_ctxt_check_job_key_reg val, 'ctxt' incl rcv */
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_job_key_reg, arg);
		if (dd->is_sriov)
			write_epsc_csr(dd, loopback_pidx_dd(dd, pidx), ctxt,
				       dd->params->send_ctxt_check_job_key_reg,
				       arg);
		if (!hfi2_is_ax(dd)) {
			dd->params->set_pio_integrity(dd, pidx, ctxt, type,
						      arg ? SPI_SET_JKEY :
							    SPI_CLEAR_JKEY);
		}
		/* Enable/clear J_KEY check on receive context. */
		if (arg) {
			/* convert sctxt jkey to rctxt */
			arg = (arg >> SEND_CTXT_CHECK_JOB_KEY_VALUE_SHIFT) &
			      SEND_CTXT_CHECK_JOB_KEY_VALUE_MASK;
			arg = RCV_KEY_CTRL_JOB_KEY_ENABLE_SMASK |
			      ((arg & RCV_KEY_CTRL_JOB_KEY_VALUE_MASK)
			       << RCV_KEY_CTRL_JOB_KEY_VALUE_SHIFT);
		}
		write_iprc_csr(dd, pidx, rctxt, dd->params->rcv_jkey_ctrl_reg,
			       arg);
		if (dd->is_sriov)
			write_iprc_csr(dd, loopback_pidx_dd(dd, pidx), rctxt,
				       dd->params->rcv_jkey_ctrl_reg, arg);
		break;
	case SC_CHK_PKEY_OP: /* 'arg' is send_ctxt_check_partition_key_reg value */
		if (!arg)
			dd->params->set_pio_integrity(dd, pidx, ctxt, type,
						      SPI_CLEAR_PKEY);
		write_epsc_csr(dd, pidx, ctxt,
			       dd->params->send_ctxt_check_partition_key_reg,
			       arg);
		if (dd->is_sriov)
			write_epsc_csr(
				dd, loopback_pidx_dd(dd, pidx), ctxt,
				dd->params->send_ctxt_check_partition_key_reg,
				arg);
		if (arg)
			dd->params->set_pio_integrity(dd, pidx, ctxt, type,
						      SPI_SET_PKEY);
		break;
	case SC_CHK_ADJ_OP: /* 'arg' is enable flag (do SC_CHK_INIT_OP also) */
		dd->params->set_pio_integrity(dd, pidx, ctxt, type,
					      SPI_DEFAULT);
		if (!arg)
			break;
		fallthrough;
	case SC_CHK_INIT_OP: /* 'arg' not used */
		dd->params->set_pio_integrity(dd, pidx, ctxt, type, SPI_INIT);
		break;
	case SC_ENABLE_OP: /* 'arg' not used as input, 'pidx' not used */
		ret = hfi2_pio_reset_one(dd, ctxt);
		if (ret)
			break;

		/*
		 * All is well. Enable the context.
		 */
		arg = read_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg);
		arg |= SC(CTRL_CTXT_ENABLE_SMASK);
		write_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg, arg);
		/*
		 * Read SendCtxtCtrl to force the write out and prevent a timing
		 * hazard where a PIO write may reach the context before the enable.
		 */
		read_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg);
		break;
	case SC_DISABLE_OP: /* 'arg' not used as input, 'pidx' not used */
		arg = read_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg);
		arg &= ~SC(CTRL_CTXT_ENABLE_SMASK);
		write_tctxt_csr(dd, ctxt, dd->params->send_ctxt_ctrl_reg, arg);
		break;
	case RC_ENABLE_OP: /* 'arg' is enable flag */
		hfi2_jkr_ena_rcv_ctxt(dd, pidx, ctxt, arg);
		if (dd->is_sriov)
			hfi2_jkr_ena_rcv_ctxt(dd, loopback_pidx_dd(dd, pidx), ctxt,
					 arg);
		break;
	case RC_HEADER_OP: /* 'arg' is size */
		hfi2_jkr_upd_rcv_hdr_size(dd, pidx, ctxt, arg);
		if (dd->is_sriov)
			hfi2_jkr_upd_rcv_hdr_size(dd, loopback_pidx_dd(dd, pidx),
					     ctxt, arg);
		break;
	case LINK_BOUNCE_OP: /* 'arg' is not used */
		queue_work(dd->pport[pidx].link_wq,
			   &dd->pport[pidx].link_bounce_work);
		break;
	}
	return ret;
}
