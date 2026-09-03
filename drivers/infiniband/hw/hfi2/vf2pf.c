// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * SRIOV support for VFs making requests to PF0.
 */

#include "hfi2.h"
#include "chip.h"
#include "chip_gen.h"
#include "mad.h"
#include "sriov.h"
#include "vf2pf_int.h"

#ifdef HFI_VF2PF_LOOPBACK
#include "vf2pf_lb.h"
#define HFI_VF2PF_LOOPBACK_CONFIG
#endif

static bool vf2pf_lb = true;

static uint vf2pf_to = 1;

#define VF2PF_FORCE_LB /* set to force use of hfi2_loopback vf2pf even if VFs are local */

#ifdef VF2PF_FORCE_LB
#define IS_LOCAL_VF(dd) (!vf2pf_lb && !(dd)->is_vm)
#define IS_LOCAL_VDD(vdd) (!vf2pf_lb && (vdd))
#else
#define IS_LOCAL_VF(dd) (!(dd)->is_vm)
#define IS_LOCAL_VDD(vdd) (vdd)
#endif

static struct vf2pf_devops vf2pf_nodev = {};

static struct vf2pf_devops *vf2pf_dev = &vf2pf_nodev;

/* for additional output to "hw_resources" */
int hfi2_vf2pf_sysfs_emit_at(struct hfi2_devdata *dd, char *buf, int at)
{
	int off = at;

	/*
	 * Anything for vf2pf core goes here.
	 */

	if (vf2pf_dev->sysfs_emit_at)
		off += vf2pf_dev->sysfs_emit_at(dd, buf, off);

	return off - at;
}

/*
 * Allocate memory for a vf2pf message to transmit.
 * Returns pointer to allocation, to be used in kfree() and
 * passing to vf2pf_devops.send().
 *
 * On success,
 * 'msg' is set to struct vf2pf_hdr (vf2pf payload) part of allocation,
 *
 * buffer contents/structure:
 *
 * ret->	struct vf2pf_prefix
 *		-align u64-
 *		[opt: implimentation headers]
 * msg->	struct vf2pf_hdr
 *		variable payload...
 */
static void *msg_alloc(struct hfi2_devdata *dd, struct vf2pf_hdr **msg)
{
	if (!vf2pf_dev->msg_alloc)
		return NULL;

	return vf2pf_dev->msg_alloc(dd, msg);
}

/*
 * Send a message to 'si'.
 *
 * 'buf' is opaque pointer returned by msg_alloc().
 * header part (vf2pf_dev->get_msg(dd, buf)) must have been filled out.
 * caller may kfree on return.
 */
static int vf2pf_send(struct hfi2_devdata *dd, u8 si, void *buf)
{
	if (!vf2pf_dev->send)
		return -ENXIO;

	return vf2pf_dev->send(dd, si, buf);
}

/*
 * overwrites 'buf' with response.
 * caller acquired 'buf' via msg_alloc().
 * on success, 'buf' contains the response (caller kfrees when done).
 */
static int vf2pf_send_recv(struct hfi2_devdata *dd, u8 si, void *buf, long to)
{
	struct vf2pf_prefix *pfx = buf;
	struct vf2pf_hdr *hdr;
	int ret;

	if (!vf2pf_dev->set_tid || !vf2pf_dev->get_msg)
		return -EINVAL;

	if (to > 0) {
		pfx->type = VF2PF_PFX_TYPE_WAIT;
		init_waitqueue_head(&pfx->wait);
	} else {
		to = -to;
		pfx->type = VF2PF_PFX_TYPE_SEMA;
		sema_init(&pfx->sema, 0);
	}
	hdr = vf2pf_dev->get_msg(dd, buf);
	hdr->tid = vf2pf_dev->set_tid(dd, buf);

	ret = vf2pf_send(dd, si, buf);
	if (ret) {
		vf2pf_dev->get_tid(dd, hdr->tid); /* discard tid */
		return ret;
	}

	if (pfx->type == VF2PF_PFX_TYPE_WAIT) {
		if (vf2pf_dev->rcv_wait) {
			ret = vf2pf_dev->rcv_wait(dd, buf, to);
		} else {
			ret = wait_event_timeout(pfx->wait,
						 (hdr->op & VF2PF_OP_RESP), to);
			ret = ret ? 0 :
				    -ETIME; /* convert residual time  to error */
		}
	} else {
		ret = down_timeout(&pfx->sema, to);
	}
	if (ret) /* timeout or other error */
		vf2pf_dev->get_tid(dd, hdr->tid); /* discard tid */

	return ret;
}

/*
 * VF call to PF0 to setup dd->rsrcs.
 */
int hfi2_vf2pf_get_config(struct hfi2_devdata *dd, struct hfi2_devrsrcs *out, int si)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_getcfg_msg *msg;
	void *mem;
	int ret;

	if (!dd->is_vf)
		return -EINVAL;
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		ret = hfi2_sriov_get_config(pdd, out, si);
		if (ret)
			return ret;
		dd->base_guid = pdd->base_guid;
		dd->revision = pdd->revision;
		dd->hfi2_id = pdd->hfi2_id;
		dd->icode = pdd->icode;
		dd->irev = pdd->irev;
		dd->cport_ver = pdd->cport_ver;
		return 0;
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_getcfg_msg *)hdr;
	msg->hdr.op = VF2PF_GET_CFG;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->si = si;
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (!ret)
		ret = hdr->status;
	if (ret)
		goto out;
	memcpy(out, &msg->rsrcs, sizeof(*out));
	dd->base_guid = msg->base_guid;
	dd->revision = msg->revision;
	dd->hfi2_id = msg->hfi2_id;
	dd->icode = msg->icode;
	dd->irev = msg->irev;
	dd->cport_ver = msg->cport_ver;
out:
	kfree(mem);
	return ret;
}

static int do_asgnrs_msg(struct hfi2_devdata *dd, u8 op,
			 struct hfi2_devrsrcs *vfr)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_asgnrs_msg *msg;
	void *mem;
	int ret;

	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_asgnrs_msg *)hdr;
	msg->hdr.op = op;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	memcpy(&msg->rsrcs, vfr, sizeof(msg->rsrcs));
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (!ret)
		ret = hdr->status;
	kfree(mem);
	return ret;
}

/*
 * VF call to PF0 to assign chip resources to this SI.
 * May include additional early setup.
 */
int hfi2_vf2pf_assign_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *vfr)
{
	if (!dd->is_vf)
		return -EINVAL;
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_sriov_assign_rsrcs(pdd, vfr);
	}
	return do_asgnrs_msg(dd, VF2PF_ASGN_RES, vfr);
}

/*
 * VF call to PF0 to release chip resources.
 * May include other late shutdown.
 */
int hfi2_vf2pf_free_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *vfr)
{
	if (!dd->is_vf)
		return -EINVAL;
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		hfi2_sriov_free_rsrcs(pdd, vfr);
		return 0;
	}
	return do_asgnrs_msg(dd, VF2PF_FREE_RES, vfr);
}

int hfi2_vf2pf_priv_reg_op(struct hfi2_devdata *dd, int pidx, u32 ctxt, int type,
		      enum preg_op op, u64 arg)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_pregop_msg *msg;
	void *mem;
	int ret;

	if (!dd->is_vf)
		return -EINVAL;
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_priv_reg_op(pdd, pidx, ctxt, type, op, arg);
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_pregop_msg *)hdr;
	msg->hdr.op = VF2PF_PREG_OP;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->pidx = pidx;
	msg->ctxt = ctxt;
	msg->type = type;
	msg->op = op;
	msg->arg = arg;
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (!ret)
		ret = hdr->status;
	kfree(mem);
	return ret;
}

/* Called for PF0 and VFs */
u64 hfi2_pf0_read_csr(struct hfi2_devdata *dd, enum csr_type type, u32 off, u16 ctxt,
		 u8 pidx_eng)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_readcsr_msg *msg;
	void *mem;
	u64 reg = ~(u64)0; /* error */
	int ret;

	if (!dd->is_vf)
		return hfi2_read_csr(dd, off);
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_read_csr_type(pdd, type, off, ctxt, pidx_eng);
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_readcsr_msg *)hdr;
	msg->hdr.op = VF2PF_RCSR_OP;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->off = off;
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (!ret)
		ret = hdr->status;
	if (!ret)
		reg = msg->reg;
	kfree(mem);
	return reg;
}

/* Only called for VFs */
u64 hfi2_pf0_rctxt_ctrl_op(struct hfi2_devdata *dd, u16 ctxt, unsigned int op)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_rcctrl_msg *msg;
	void *mem;
	u64 reg = ~(u64)0; /* error */
	int ret;

	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_rctxt_ctrl_op(pdd, ctxt, op);
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_rcctrl_msg *)hdr;
	msg->hdr.op = VF2PF_RCCTRL_OP;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->ctxt = ctxt;
	msg->op = op;
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (!ret)
		ret = hdr->status;
	if (!ret)
		reg = msg->reg;
	kfree(mem);
	return reg;
}

void hfi2_vf2pf_tid_config(struct hfi2_devdata *dd, int pidx, u16 ctxt,
		      u32 eager_base, u16 alloced, u32 expected_base,
		      u32 expected_count)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_tidcfg_msg *msg;
	void *mem;

	if (!dd->is_vf)
		return; /*should never happen */
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		pdd->params->set_port_tid_config(pdd, pidx, ctxt, eager_base,
						 alloced, expected_base,
						 expected_count);
		return;
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem) {
		dd_dev_err(
			dd,
			"Failed to allocate vf2pf message buffer for tid_config\n");
		return;
	}
	msg = (struct vf2pf_tidcfg_msg *)hdr;
	msg->hdr.op = VF2PF_TIDCFG_OP;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->pidx = pidx;
	msg->ctxt = ctxt;
	msg->alloced = alloced;
	msg->egr_base = eager_base;
	msg->exp_base = expected_base;
	msg->exp_cnt = expected_count;
	vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	kfree(mem);
}

int hfi2_vf2pf_init_rxe_rsm(struct hfi2_devdata *dd)
{
	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_init_rxe_rsm(pdd, &dd->rsrcs);
	}
	return do_asgnrs_msg(dd, VF2PF_RXERSM_OP, &dd->rsrcs);
}

u16 hfi2_vf2pf_get_qp_map(struct hfi2_devdata *dd, int pidx, u16 idx)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_qpmap_msg *msg;
	void *mem;
	u16 res = 0; /* guaranteed error */
	int ret;

	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_get_qp_map(pdd->pport + pidx, idx);
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_qpmap_msg *)hdr;
	msg->hdr.op = VF2PF_QPMAP_OP;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->pidx = pidx;
	msg->idx = idx;
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (!ret)
		ret = hdr->status;
	if (!ret)
		res = msg->res;
	kfree(mem);
	return res;
}

/*
 * called on PF0 to distribute port_info to all VFs.
 */
int hfi2_pf2vf_push_portinfo(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			struct opa_port_info *pi, int si_mask)
{
	struct hfi2_devdata *dd = ppd->dd, *vdd;
	struct pci_dev *pdev, *vpdev;
	struct vf2pf_hdr *hdr;
	struct pf0_pushpi_msg *msg;
	void *mem;
	int id;
	int ret;

	if (dd->is_vf)
		return -EINVAL;

	if (si_mask == VF2PF_SI_ALL)
		si_mask = dd->rsrcs.sync_done;
	if (!si_mask)
		return 0;

	pdev = dd->pcidev;
	/*
	 * However, it is possible that some VFs may be local and some in VMs,
	 * so might need to have each VF differently. At least, though, we can
	 * only allocate message buffer once.
	 */
	/*
	 * 'pi' is always opa_get_smp_data(smp) so we only
	 * need to send 'smp' (the whole MAD).
	 */
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct pf0_pushpi_msg *)hdr;
	msg->hdr.op = PF0_PUSH_PI;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->pidx = ppd->hw_pidx;
	memcpy(&msg->smp, smp, sizeof(*smp));
	for (id = 0; id < dd->rsrcs.num_vfs; ++id) {
		if (!(si_mask & (1 << (id + 1))))
			continue;
		/*
		 * pci/iov.c uses pci_iov_virtfn_bus(pdev, id) but we don't have that,
		 * will pdev->bus->number work?
		 */
		vpdev = pci_get_domain_bus_and_slot(
			pci_domain_nr(pdev->bus), pdev->bus->number,
			pci_iov_virtfn_devfn(pdev, id));
		if (!vpdev)
			continue; /* error or just skip? */
		vdd = pci_get_drvdata(vpdev);
		if (IS_LOCAL_VDD(vdd)) { /* must not be in VM... */
			ret = hfi2_update_from_opa_portinfo(
				&vdd->pport[ppd->hw_pidx], smp, pi);
		} else {
			ret = vf2pf_send(dd, id + 1, mem);
			if (ret)
				dd_dev_warn(
					dd,
					"Failed to push portinfo to %d (%d)\n",
					id + 1, ret);
		}
		if (ret)
			break;
	}
	kfree(mem);
	return ret;
}

/*
 * called on PF0 to distribute sc2vlt to all VFs.
 */
int hfi2_pf2vf_push_sc2vlt(struct hfi2_pportdata *ppd, int si_mask)
{
	struct hfi2_devdata *dd = ppd->dd, *vdd;
	struct pci_dev *pdev, *vpdev;
	struct vf2pf_hdr *hdr;
	struct pf0_pushvlt_msg *msg;
	void *mem;
	int id;
	int ret = 0;

	if (dd->is_vf)
		return -EINVAL;

	if (si_mask == VF2PF_SI_ALL)
		si_mask = dd->rsrcs.sync_done;
	if (!si_mask)
		return 0;

	pdev = dd->pcidev;
	/*
	 * However, it is possible that some VFs may be local and some in VMs,
	 * so might need to have each VF differently. At least, though, we can
	 * only allocate message buffer once.
	 */
	/*
	 * 'pi' is always opa_get_smp_data(smp) so we only
	 * need to send 'smp' (the whole MAD).
	 */
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct pf0_pushvlt_msg *)hdr;
	msg->hdr.op = PF0_PUSH_VLT;
	msg->hdr.len = sizeof(*msg) - sizeof(*hdr);
	msg->pidx = ppd->hw_pidx;
	memcpy(msg->sc2vl, ppd->sc2vl, sizeof(msg->sc2vl));
	for (id = 0; id < dd->rsrcs.num_vfs; ++id) {
		if (!(si_mask & (1 << (id + 1))))
			continue;
		/*
		 * pci/iov.c uses pci_iov_virtfn_bus(pdev, id) but we don't have that,
		 * will pdev->bus->number work?
		 */
		vpdev = pci_get_domain_bus_and_slot(
			pci_domain_nr(pdev->bus), pdev->bus->number,
			pci_iov_virtfn_devfn(pdev, id));
		if (!vpdev)
			continue; /* error or just skip? */
		vdd = pci_get_drvdata(vpdev);
		if (IS_LOCAL_VDD(vdd)) { /* must not be in VM... */
			hfi2_update_sc2vlt(&vdd->pport[ppd->hw_pidx],
					   ppd->sc2vl, false);
		} else {
			ret = vf2pf_send(dd, id + 1, mem);
			if (ret)
				dd_dev_warn(
					dd,
					"Failed to push sc2vlt to %d (%d)\n",
					id + 1, ret);
		}
		if (ret)
			break;
	}
	kfree(mem);
	return ret;
}

int hfi2_vf2pf_send_only_mad(struct hfi2_devdata *dd, u8 sb, const void *mad,
			int len)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_mad *msg;
	void *mem;
	int ret;

	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_cport_send_only_mad(pdd, sb, mad, len);
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_mad *)hdr;
	msg->hdr.op = VF2PF_MAD_SND;
	msg->hdr.len = len + VF2PF_MAD_OVERHEAD;
	msg->sb = sb;
	memcpy(&msg->mad, mad, len);
	ret = vf2pf_send(dd, 0, mem);
	if (!ret)
		ret = hdr->status;
	kfree(mem);
	return ret;
}

int hfi2_vf2pf_send_recv_mad(struct hfi2_devdata *dd, u8 sb, const void *mad,
			int len, void *omad, size_t *omad_len, long to)
{
	struct vf2pf_hdr *hdr;
	struct vf2pf_mad *msg;
	void *mem;
	int ret;

	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		return hfi2_cport_send_recv_mad(pdd, sb, mad, len, omad, omad_len);
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;
	msg = (struct vf2pf_mad *)hdr;
	msg->hdr.op = VF2PF_MAD_SNDRCV;
	msg->hdr.len = len + VF2PF_MAD_OVERHEAD;
	msg->sb = sb;
	memcpy(&msg->mad, mad, len);
	ret = vf2pf_send_recv(dd, 0, mem, -to); /* -to: signal use semaphore */
	if (!ret)
		ret = hdr->status;
	if (!ret) {
		int olen = msg->hdr.len - VF2PF_MAD_OVERHEAD;

		if (olen > *omad_len) {
			dd_dev_warn(
				dd,
				"VF2PF MAD resp length 0x%x > 0x%lx, truncating\n",
				olen, *omad_len);
			olen = *omad_len;
		}
		memcpy(omad, &msg->mad, olen);
	}
	kfree(mem);
	return ret;
}

static void vf2pf_syncup(struct hfi2_devdata *dd, int si)
{
	atomic_or(1 << si, &dd->rsrcs.sync_pending);
	queue_work(dd->hfi2_wq, &dd->sync_vf_work);
}

static void vf2pf_sync_fn(struct work_struct *work)
{
	int ret;

	struct hfi2_devdata *dd =
		container_of(work, struct hfi2_devdata, sync_vf_work);
	int sync_pending = atomic_fetch_and(0, &dd->rsrcs.sync_pending);

	if (!sync_pending)
		return;

	dd_dev_info(dd, "syncing VFs %02x\n", sync_pending);
	dd->rsrcs.sync_done |= sync_pending;
	ret = hfi2_sriov_sync_ports(dd, sync_pending);
	if (ret)
		dd_dev_err(dd, "Failed to sync ports to %02x (%d)\n",
			   sync_pending, ret);
}

/*
 * received responses handled elsewhere.
 * 'buf' (and 'hdr') are allocated memory.
 * must be safe to destroy 'buf' on return.
 */
void hfi2_vf2pf_rcv_msg(struct hfi2_devdata *dd, struct vf2pf_hdr *hdr, void *buf)
{
	int ret = 0;

	/* hdr == vf2pf_dev->get_msg(dd, buf) */
	switch (hdr->op) {
	case VF2PF_OP_PING: {
		struct vf2pf_ping_msg *ping = (struct vf2pf_ping_msg *)hdr;

		dd_dev_info(dd, "vf2pf ping-pong with %u \"%.*s\"\n", hdr->si,
			    hdr->len, ping->data);
		if (hdr->len >= sizeof(ping->data))
			hdr->len = sizeof(ping->data) - 1;
		ping->data[hdr->len++] = '!';
		break;
	}

	/* only received on PF0 */
	case VF2PF_GET_CFG: {
		struct vf2pf_getcfg_msg *msg = (struct vf2pf_getcfg_msg *)hdr;

		ret = hfi2_sriov_get_config(dd, &msg->rsrcs, msg->si);
		/* copy these even if error */
		msg->base_guid = dd->base_guid;
		msg->revision = dd->revision;
		msg->hfi2_id = dd->hfi2_id;
		msg->icode = dd->icode;
		msg->irev = dd->irev;
		msg->cport_ver = dd->cport_ver;
		break;
	}
	case VF2PF_ASGN_RES: {
		struct vf2pf_asgnrs_msg *msg = (struct vf2pf_asgnrs_msg *)hdr;

		ret = hfi2_sriov_assign_rsrcs(dd, &msg->rsrcs);
		break;
	}
	case VF2PF_FREE_RES: {
		struct vf2pf_asgnrs_msg *msg = (struct vf2pf_asgnrs_msg *)hdr;

		hfi2_sriov_free_rsrcs(dd, &msg->rsrcs);
		break;
	}
	case VF2PF_PREG_OP: {
		struct vf2pf_pregop_msg *msg = (struct vf2pf_pregop_msg *)hdr;

		ret = hfi2_priv_reg_op(dd, msg->pidx, msg->ctxt, msg->type, msg->op,
				  msg->arg);
		break;
	}
	case VF2PF_RCSR_OP: {
		struct vf2pf_readcsr_msg *msg = (struct vf2pf_readcsr_msg *)hdr;

		msg->reg = hfi2_read_csr(dd, msg->off);
		break;
	}
	case VF2PF_RCCTRL_OP: {
		struct vf2pf_rcctrl_msg *msg = (struct vf2pf_rcctrl_msg *)hdr;

		ret = hfi2_rctxt_ctrl_op(dd, msg->ctxt, msg->op);
		break;
	}
	case VF2PF_TIDCFG_OP: {
		struct vf2pf_tidcfg_msg *msg = (struct vf2pf_tidcfg_msg *)hdr;

		dd->params->set_port_tid_config(dd, msg->pidx, msg->ctxt,
						msg->egr_base, msg->alloced,
						msg->exp_base, msg->exp_cnt);
		break;
	}
	case VF2PF_RXERSM_OP: {
		struct vf2pf_asgnrs_msg *msg = (struct vf2pf_asgnrs_msg *)hdr;

		ret = hfi2_init_rxe_rsm(dd, &msg->rsrcs);
		break;
	}
	case VF2PF_QPMAP_OP: {
		struct vf2pf_qpmap_msg *msg = (struct vf2pf_qpmap_msg *)hdr;

		msg->res = hfi2_get_qp_map(dd->pport + msg->pidx, msg->idx);
		break;
	}
	case VF2PF_STOP: {
		if (vf2pf_dev->deinit)
			vf2pf_dev->deinit(dd, hdr->si);
		return; /* no response */
	}
	case VF2PF_MAD_SND: {
		struct vf2pf_mad *msg = (struct vf2pf_mad *)hdr;

		ret = hfi2_cport_send_only_mad(dd, msg->sb, &msg->mad,
					  msg->hdr.len - VF2PF_MAD_OVERHEAD);
		if (ret)
			dd_dev_err(dd, "Failed to send MAD to CPORT (%d)\n",
				   ret);
		return; /* no response */
	}
	case VF2PF_MAD_SNDRCV: {
		struct vf2pf_mad *msg = (struct vf2pf_mad *)hdr;
		size_t olen = sizeof(msg->mad);

		ret = hfi2_cport_send_recv_mad(dd, msg->sb, &msg->mad,
					  msg->hdr.len - VF2PF_MAD_OVERHEAD,
					  &msg->mad, &olen);
		if (!ret)
			msg->hdr.len = olen;
		break;
	}
	case VF2PF_READY: {
		vf2pf_syncup(dd, hdr->si);
		return; /* no response */
	}

	/* only received on VFs */
	case PF0_PUSH_PI: {
		struct pf0_pushpi_msg *msg = (struct pf0_pushpi_msg *)hdr;
		struct opa_port_info *pi =
			(struct opa_port_info *)opa_get_smp_data(&msg->smp);

		ret = hfi2_update_from_opa_portinfo(dd->pport + msg->pidx, &msg->smp,
					       pi);
		if (ret)
			dd_dev_err(dd,
				   "Failed to process port_info update (%d)\n",
				   ret);
		return; /* no response */
	}
	case PF0_PUSH_VLT: {
		struct pf0_pushvlt_msg *msg = (struct pf0_pushvlt_msg *)hdr;

		hfi2_update_sc2vlt(dd->pport + msg->pidx, msg->sc2vl, false);
		return; /* no response */
	}
	default:
		dd_dev_err(dd, "Unknown vf2pf msg op %u from %u\n", hdr->op,
			   hdr->si);
		return;
	}

	/* reaching here means response is to be sent */
	hdr->op |= VF2PF_OP_RESP;
	hdr->status = ret;
	vf2pf_send(dd, hdr->si, buf);
}

/*
 * 'buf' may point to h/w recv buffer.
 * called from intr context: must expedite handling.
 * hdr->op has VF2PF_OP_RESP set, in order to reach here.
 */
void hfi2_vf2pf_rsp_msg(struct hfi2_devdata *dd, void *buf)
{
	struct vf2pf_hdr *hdr = buf;
	struct vf2pf_prefix *wpfx; /* msg object waiting for response */
	struct vf2pf_hdr *whdr; /* hdr waiting for response */
	void *wbuf;

	wpfx = vf2pf_dev->get_tid(dd, hdr->tid);
	if (!wpfx) {
		dd_dev_err(dd, "vf2pf response op %x has no waiter\n", hdr->op);
		return;
	}
	whdr = vf2pf_dev->get_msg(dd, wpfx);
	wbuf = whdr;
	/*
	 * need to avoid race by setting VF2PF_OP_RESP in whdr->op last (after barrier).
	 * this requires that VF2PF_OP_RESP is cleared in 'op' before the memcpy,
	 * then set after in the destination.
	 */
	hdr->op &= ~VF2PF_OP_RESP;
	memcpy(wbuf, buf, hdr->len + sizeof(*hdr));
	smp_wmb(); /* needed? */
	whdr->op |= VF2PF_OP_RESP; /* to trigger wakeup condition */
	if (wpfx->type == VF2PF_PFX_TYPE_WAIT)
		wake_up(&wpfx->wait);
	else
		up(&wpfx->sema);
}

/*
 * Return number of special contexts needed by implementation.
 * Should be either 0 or JKR_C_CCE_NUM_VFS + 1.
 */
int hfi2_vf2pf_num_ctxts(struct hfi2_devdata *dd)
{
	if (dd->params->chip_type == CHIP_WFR || !dd->is_sriov)
		return 0;
	return vf2pf_dev->num_ctxts;
}

int hfi2_vf2pf_num_irq(struct hfi2_devdata *dd)
{
	if (dd->params->chip_type == CHIP_WFR || !dd->is_sriov)
		return 0;
	return vf2pf_dev->num_irq;
}

/* returns 0 on error (invalid VF SI) */
int hfi2_vf2pf_probe_si(struct hfi2_devdata *dd)
{
	if (vf2pf_dev->probe_si)
		return vf2pf_dev->probe_si(dd);
	return 0;
}

static ssize_t vf2pf_ping_store(struct device *device,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	void *mem;
	struct vf2pf_hdr *hdr;
	struct vf2pf_ping_msg *ping;
	size_t len = count;
	int ret;

	if (count > sizeof(ping->data))
		return -EINVAL;

	/* trim one newline if present */
	if (buf[len - 1] == '\n')
		--len;

	mem = msg_alloc(dd, &hdr);
	if (!mem)
		return -ENOMEM;

	ping = (struct vf2pf_ping_msg *)hdr;
	ping->hdr.op = VF2PF_OP_PING;
	ping->hdr.len = len;
	memcpy(ping->data, buf, len);

	dd_dev_info(dd, "vf2pf ping 0 \"%.*s\"\n", ping->hdr.len, buf);
	ret = vf2pf_send_recv(dd, 0, mem, vf2pf_to * HZ);
	if (ret)
		dd_dev_warn(dd, "vf2pf ping send failed (%d)\n", ret);
	else
		dd_dev_info(dd, "vf2pf ping resp from %u \"%.*s\"\n",
			    ping->hdr.si, ping->hdr.len, (char *)ping->data);
	kfree(mem);
	return count;
}

static DEVICE_ATTR_WO(vf2pf_ping);

static ssize_t vf2pf_sync_store(struct device *device,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	unsigned long sync_mask;
	int ret;

	/* allow 0 mask and trigger work func anyway */
	ret = kstrtoul(buf, 0, &sync_mask);
	if (ret || (sync_mask & ~0b011111110))
		return -EINVAL;

	atomic_or(sync_mask, &dd->rsrcs.sync_pending);
	queue_work(dd->hfi2_wq, &dd->sync_vf_work);

	return count;
}

static DEVICE_ATTR_WO(vf2pf_sync);

void hfi2_vf2pf_set_si_enables(struct hfi2_devdata *dd, int si, u64 *csrs,
			  void (*si_enables)(struct hfi2_devdata *dd, u64 *csrs,
					     u32 start, u32 end))
{
	if (!vf2pf_dev->set_si_enables)
		return;
	vf2pf_dev->set_si_enables(dd, si, csrs, si_enables);
}

void hfi2_vf2pf_ready(struct hfi2_devdata *dd)
{
	struct vf2pf_hdr *hdr;
	void *mem;
	int ret;

	if (!dd->is_vf)
		return;

	if (IS_LOCAL_VF(
		    dd)) { /* VF and PF0 are using the same driver/OS instance */
		struct hfi2_devdata *pdd = pci_get_drvdata(dd->pcidev->physfn);

		vf2pf_syncup(pdd, dd->rsrcs.si_idx);
		return;
	}
	mem = msg_alloc(dd, &hdr);
	if (!mem) {
		dd_dev_err(dd, "Failed to signal ready to PF0 (msg_alloc)\n");
		return;
	}
	hdr->op = VF2PF_READY;
	hdr->len = 0;
	ret = vf2pf_send(dd, 0, mem);
	kfree(mem);
	if (ret)
		dd_dev_err(dd, "Failed to signal ready to PF0 (%d)\n", ret);
}

void hfi2_vf2pf_init_sysfs(struct hfi2_devdata *dd, struct device *class_dev)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	int ret;

	if (!lbd)
		return;

	if (dd->is_vf) {
		ret = sysfs_create_file(&class_dev->kobj,
					&dev_attr_vf2pf_ping.attr);
		if (ret)
			dd_dev_warn(dd, "failed to create sysfs attr %s (%d)\n",
				    dev_attr_vf2pf_ping.attr.name, ret);
	} else {
		ret = sysfs_create_file(&class_dev->kobj,
					&dev_attr_vf2pf_sync.attr);
		if (ret)
			dd_dev_warn(dd, "failed to create sysfs attr %s (%d)\n",
				    dev_attr_vf2pf_sync.attr.name, ret);
	}
	if (vf2pf_dev->init_sysfs)
		vf2pf_dev->init_sysfs(dd, class_dev);
}

int hfi2_vf2pf_init_irq(struct hfi2_devdata *dd)
{
	if (!vf2pf_dev->init_irq)
		return 0;
	return vf2pf_dev->init_irq(dd);
}

void hfi2_vf2pf_deinit_irq(struct hfi2_devdata *dd)
{
	if (!vf2pf_dev->deinit_irq)
		return;
	vf2pf_dev->deinit_irq(dd);
}

/*
 * This is called on PF0 only, just before creation of VFs.
 *
 * This may be called multiple times throughout the life of the PF0
 * driver, if VFs are destroyed and recreated.
 */
int hfi2_vf2pf_prep(struct hfi2_devdata *dd)
{
	if (!vf2pf_dev->init)
		return 0;
	return vf2pf_dev->init(dd, VF2PF_INIT_ALL);
}

/*
 * This is called early in the initialization.
 * It must not depend on any SRIOV configuration being setup,
 * but may call into the sriov module to decide if SRIOV is allowed.
 */
int hfi2_vf2pf_early_init(struct hfi2_devdata *dd)
{
	if (dd->params->chip_type == CHIP_WFR || !dd->is_sriov)
		return 0;

	if (!dd->is_vf) {
		INIT_WORK(&dd->sync_vf_work, vf2pf_sync_fn);
		atomic_set(&dd->rsrcs.sync_pending, 0);
		dd->rsrcs.sync_done = 0;
	}

#ifdef HFI_VF2PF_LOOPBACK
#ifndef HFI_VF2PF_LOOPBACK_CONFIG
	if (vf2pf_lb)
#endif
		vf2pf_dev = hfi2_get_lb_devops();
#endif
	/* this may require BARs, must have been mapped by now */
	return hfi2_sriov_set_si(dd);
}

/*
 * This does the actual vf2pf implementation init,
 * which may need to be done later.
 */
int hfi2_vf2pf_init(struct hfi2_devdata *dd)
{
	if (dd->params->chip_type == CHIP_WFR || !dd->is_sriov)
		return 0;

	if (!vf2pf_dev->init)
		return 0;
	return vf2pf_dev->init(dd, dd->rsrcs.si_idx);
}

/*
 * On VFs, this only sends a notification to PF0.
 * On PF0, this does a full de-initialization.
 */
void hfi2_vf2pf_deinit(struct hfi2_devdata *dd)
{
	struct vf2pf_hdr *hdr;
	void *mem;
	int ret;

	if (dd->is_vf) {
		if (IS_LOCAL_VF(
			    dd)) { /* VF and PF0 are using the same driver/OS instance */
			struct hfi2_devdata *pdd =
				pci_get_drvdata(dd->pcidev->physfn);

			if (vf2pf_dev->deinit)
				vf2pf_dev->deinit(pdd, dd->rsrcs.si_idx);
			goto out;
		}
		mem = msg_alloc(dd, &hdr);
		if (!mem) {
			dd_dev_err(dd, "Failed to notify PF0 (msg_alloc)\n");
			return;
		}
		hdr->op = VF2PF_STOP;
		hdr->len = 0;
		ret = vf2pf_send(dd, 0, mem);
		kfree(mem);
		if (ret)
			dd_dev_err(dd, "Failed to notify PF0 (%d)\n", ret);
	}
out:
	if (vf2pf_dev->deinit)
		vf2pf_dev->deinit(dd, dd->rsrcs.si_idx);
}
