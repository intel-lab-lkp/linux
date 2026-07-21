// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU Admin Function driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */

#include <linux/bitfield.h>
#include "rvu.h"
#include "rvu_sw.h"
#include "rvu_sw_l2.h"
#include "rvu_sw_l3.h"
#include "rvu_sw_fl.h"

u32 rvu_sw_port_id(struct rvu *rvu, u16 pcifunc)
{
	u16 rep_id;

	if (!rvu->rep2pfvf_map || !rvu->rep_cnt)
		return RVU_SW_INVALID_PORT_ID;

	rep_id = rvu_rep_get_vlan_id(rvu, pcifunc);
	if (rep_id >= rvu->rep_cnt ||
	    rvu->rep2pfvf_map[rep_id] != pcifunc)
		return RVU_SW_INVALID_PORT_ID;

	return FIELD_PREP(GENMASK_ULL(31, 16), rep_id) |
	       FIELD_PREP(GENMASK_ULL(15, 0), pcifunc);
}

static bool rvu_sw_swdev2af_msg_valid(u64 msg_type)
{
	return msg_type == SWDEV2AF_MSG_TYPE_FW_STATUS ||
	       msg_type == SWDEV2AF_MSG_TYPE_REFRESH_FDB ||
	       msg_type == SWDEV2AF_MSG_TYPE_REFRESH_FL;
}

static int rvu_sw_swdev2af_sender_check(struct rvu *rvu,
					struct swdev2af_notify_req *req,
					u64 msg_type)
{
	u16 sender = req->hdr.pcifunc;

	if (!rvu_sw_swdev2af_msg_valid(msg_type))
		return -EINVAL;

	if (!rvu_is_switch_pcifunc(rvu, sender))
		return -EPERM;

	return 0;
}

int rvu_mbox_handler_swdev2af_notify(struct rvu *rvu,
				     struct swdev2af_notify_req *req,
				     struct msg_rsp *rsp)
{
	int rc;

	rc = rvu_sw_swdev2af_sender_check(rvu, req, req->msg_type);
	if (rc)
		return rc;

	switch (req->msg_type) {
	case SWDEV2AF_MSG_TYPE_FW_STATUS:
		rc = rvu_sw_l2_init_offl_wq(rvu, req->hdr.pcifunc, req->fw_up);
		break;

	case SWDEV2AF_MSG_TYPE_REFRESH_FDB:
		rc = rvu_sw_l2_fdb_list_entry_add(rvu, req->pcifunc, req->mac);
		break;

	default:
		rc = -EOPNOTSUPP;
		break;
	}

	return rc;
}

void rvu_sw_shutdown(void)
{
	rvu_sw_l2_shutdown();
	rvu_sw_l3_shutdown();
}
