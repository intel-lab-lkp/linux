// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) MediaTek TinySYS Protocol
 *
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#define pr_fmt(fmt) "SCMI Notifications TinySYS - " fmt

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scmi_protocol.h>
#include <linux/scmi_mtk_protocol.h>

#include "../../protocols.h"
#include "../../notify.h"

#define SCMI_MTK_CMD_SSPM_QUERY_ALIVE		0xdead

enum scmi_mtk_tinysys_protocol_cmd {
	MTK_TINYSYS_COMMON_SET = 0x3,
	MTK_TINYSYS_COMMON_GET = 0x4,
	MTK_TINYSYS_POWER_STATE_NOTIFY = 0x5,
	MTK_TINYSYS_SLBC_CTRL = 0x6,
};

struct scmi_mtk_tinysys_common_get_payld {
	__le32 rsvd;
	__le32 param[SCMI_MTK_MSG_COMMON_REPLY_BYTES];
};

struct scmi_mtk_tinysys_common_set_payld {
	__le32 rsvd;
	__le32 ctrl_id;
	__le32 param[SCMI_MTK_MSG_COMMON_PARAM_BYTES];
};

struct scmi_mtk_tinysys_slbc_payld {
	__le32 rsvd;
	__le32 cmd;
	__le32 arg[SCMI_MTK_MSG_SLBC_PARAM_BYTES];
};

struct scmi_mtk_tinysys_pwrst_notify {
	__le32 rsvd;
	__le32 fid;
	__le32 enable;
};

struct scmi_mtk_tinysys_notify_payld {
	__le32 fid;
	__le32 param[SCMI_MTK_MSG_NOTIF_ST_BYTES];
};

struct scmi_mtk_tinysys_protocol_attributes {
	__le32 attributes;
};

struct scmi_mtk_tinysys_info {
	int num_domains;
};

static int scmi_mtk_tinysys_attributes_get(const struct scmi_protocol_handle *ph,
					   struct scmi_mtk_tinysys_info *tinfo)
{
	struct scmi_mtk_tinysys_protocol_attributes *attr;
	struct scmi_xfer *t;
	int ret;

	ret = ph->xops->xfer_get_init(ph, PROTOCOL_ATTRIBUTES, 0, sizeof(*attr), &t);
	if (ret)
		return ret;

	attr = t->rx.buf;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		attr->attributes = get_unaligned_le32(t->rx.buf);
		tinfo->num_domains = attr->attributes;
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int scmi_mtk_tinysys_get_num_sources(const struct scmi_protocol_handle *ph)
{
	struct scmi_mtk_tinysys_info *tinfo = ph->get_priv(ph);

	if (!tinfo)
		return -EINVAL;

	return tinfo->num_domains;
}

static int scmi_mtk_tinysys_set_notify_enabled(const struct scmi_protocol_handle *ph,
					       u8 evt_id, u32 src_id, bool enable)
{
	struct scmi_mtk_tinysys_pwrst_notify *pwrst_notify;
	struct scmi_xfer *t;
	int ret;

	/* There's only one possible event for now */
	if (evt_id != 0)
		return -EINVAL;

	ret = ph->xops->xfer_get_init(ph, MTK_TINYSYS_POWER_STATE_NOTIFY,
				      sizeof(*pwrst_notify), 0, &t);
	if (ret)
		return ret;

	pwrst_notify = t->tx.buf;
	pwrst_notify->fid = src_id;
	pwrst_notify->enable = cpu_to_le32(enable);

	ret = ph->xops->do_xfer(ph, t);
	ph->xops->xfer_put(ph, t);
	return ret;
}

static void *scmi_mtk_tinysys_fill_custom_report(const struct scmi_protocol_handle *ph,
						 u8 evt_id, ktime_t timestamp,
						 const void *payld, size_t payld_sz,
						 void *report, u32 *src_id)
{
	const struct scmi_mtk_tinysys_notify_payld *p = payld;
	struct scmi_mtk_tinysys_notif_report *r = report;
	int i;

	if (sizeof(*p) != payld_sz)
		return NULL;

	if (evt_id == SCMI_EVENT_MTK_TINYSYS_NOTIFIER) {
		r->timestamp = timestamp;
		r->fid = le32_to_cpu(p->fid);
		for (i = 0; i < SCMI_MTK_MSG_NOTIF_ST_BYTES; i++)
			r->status[i] = le32_to_cpu(p->param[i]);
		if (src_id)
			*src_id = p->fid;
	} else {
		WARN_ON_ONCE(1);
		return NULL;
	}

	return r;
}

static const struct scmi_event scmi_mtk_tinysys_events[] = {
	{
		.id = SCMI_EVENT_MTK_TINYSYS_NOTIFIER,
		.max_payld_sz =	sizeof(struct scmi_mtk_tinysys_notify_payld),
		.max_report_sz = sizeof(struct scmi_mtk_tinysys_notif_report),
	},
};

static int scmi_mtk_tinysys_common_get(const struct scmi_protocol_handle *ph,
				       u32 ctrl_id, u32 cmd,
				       struct scmi_mtk_tinysys_status *retval)
{
	struct scmi_mtk_tinysys_common_get_payld *p;
	struct scmi_xfer *t;
	int ret;

	ret = ph->xops->xfer_get_init(ph, MTK_TINYSYS_COMMON_GET,
				      sizeof(*p), sizeof(*retval), &t);
	if (ret)
		return ret;

	p = t->tx.buf;
	p->param[0] = ctrl_id;
	p->param[1] = cmd;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		if (t->rx.len == sizeof(*retval))
			memcpy(retval, t->rx.buf, sizeof(*retval));
		else
			ret = -EINVAL;
	}

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_mtk_tinysys_common_set(const struct scmi_protocol_handle *ph,
				       u32 ctrl_id, const u32 *params,
				       const u8 num_params)
{
	struct scmi_mtk_tinysys_common_set_payld *p;
	struct scmi_xfer *t;
	int i, ret;

	if (!params || num_params > SCMI_MTK_MSG_COMMON_PARAM_BYTES)
		return -EINVAL;

	ret = ph->xops->xfer_get_init(ph, MTK_TINYSYS_COMMON_SET, sizeof(*p), 0, &t);
	if (ret)
		return ret;

	p = t->tx.buf;
	p->ctrl_id = cpu_to_le32(ctrl_id);
	for (i = 0; i < num_params; i++)
		p->param[i] = cpu_to_le32(params[i]);

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_mtk_tinysys_cm_mgr_set(const struct scmi_protocol_handle *ph,
				       u32 ctrl_id, u32 cmd, u32 arg)
{
	const u32 params[2] = { cmd, arg };

	return scmi_mtk_tinysys_common_set(ph, ctrl_id, params, 2);
}

static int scmi_mtk_tinysys_gpu_pwr_set(const struct scmi_protocol_handle *ph,
				       u32 ctrl_id, u8 pwr_indication, bool enable)
{
	const u32 params[2] = { pwr_indication, enable };

	return scmi_mtk_tinysys_common_set(ph, ctrl_id, params, 2);
}

static int scmi_mtk_tinysys_slbc_req(const struct scmi_protocol_handle *ph,
				     const struct scmi_mtk_tinysys_slbc *req,
				     struct scmi_mtk_tinysys_slbc *retval)
{
	struct scmi_mtk_tinysys_slbc_payld *p;
	struct scmi_xfer *t;
	int i, ret;

	ret = ph->xops->xfer_get_init(ph, MTK_TINYSYS_SLBC_CTRL,
				      sizeof(*p), sizeof(*p), &t);
	if (ret)
		return ret;

	p = t->tx.buf;
	p->cmd = cpu_to_le32(req->cmd);
	for (i = 0; i < SCMI_MTK_MSG_SLBC_PARAM_BYTES; i++)
		p->arg[i] = cpu_to_le32(req->arg[i]);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret && retval) {
		if (t->rx.len == sizeof(*retval))
			memcpy(retval, t->rx.buf, sizeof(*retval));
		else
			ret = -EINVAL;
	}

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_mtk_tinysys_sspm_mem_set(const struct scmi_protocol_handle *ph,
					 u32 ctrl_id, u32 pa, u32 mem_sz)
{
	const u32 params[2] = { pa, mem_sz };

	if (mem_sz < SZ_1M)
		return -EINVAL;

	return scmi_mtk_tinysys_common_set(ph, ctrl_id, params, 2);
}

static bool scmi_mtk_tinysys_sspm_is_alive(const struct scmi_protocol_handle *ph,
					   u32 ctrl_id)
{
	const u32 param = SCMI_MTK_CMD_SSPM_QUERY_ALIVE;
	int ret;

	ret = scmi_mtk_tinysys_common_set(ph, ctrl_id, &param, 1);

	return ret ? false : true;
}

static const struct scmi_mtk_tinysys_proto_ops mtk_tinysys_proto_ops = {
	.common_get = scmi_mtk_tinysys_common_get,
	.cm_mgr_set = scmi_mtk_tinysys_cm_mgr_set,
	.gpu_pwr_set = scmi_mtk_tinysys_gpu_pwr_set,
	.slbc_req = scmi_mtk_tinysys_slbc_req,
	.sspm_is_alive = scmi_mtk_tinysys_sspm_is_alive,
	.sspm_mem_set = scmi_mtk_tinysys_sspm_mem_set,
};

static const struct scmi_event_ops scmi_mtk_tinysys_event_ops = {
	.get_num_sources = scmi_mtk_tinysys_get_num_sources,
	.set_notify_enabled = scmi_mtk_tinysys_set_notify_enabled,
	.fill_custom_report = scmi_mtk_tinysys_fill_custom_report,
};

static const struct scmi_protocol_events scmi_mtk_tinysys_protocol_events = {
	.queue_sz = 4 * SCMI_PROTO_QUEUE_SZ,
	.ops = &scmi_mtk_tinysys_event_ops,
	.evts = scmi_mtk_tinysys_events,
	.num_events = ARRAY_SIZE(scmi_mtk_tinysys_events),
};

static int scmi_mtk_tinysys_protocol_init(const struct scmi_protocol_handle *ph)
{
	struct scmi_mtk_tinysys_info *tinfo;
	u32 version;
	int ret;

	ret = ph->xops->version_get(ph, &version);
	if (ret)
		return ret;

	dev_info(ph->dev, "MediaTek TinySYS Protocol Version %d.%d\n",
		 PROTOCOL_REV_MAJOR(version), PROTOCOL_REV_MINOR(version));

	tinfo = devm_kzalloc(ph->dev, sizeof(*tinfo), GFP_KERNEL);
	if (!tinfo)
		return -ENOMEM;

	ret = scmi_mtk_tinysys_attributes_get(ph, tinfo);
	if (ret)
		return ret;

	return ph->set_priv(ph, tinfo, version);
}

static const struct scmi_protocol scmi_mtk_tinysys = {
	.id = SCMI_PROTOCOL_MTK_TINYSYS,
	.owner = THIS_MODULE,
	.instance_init = &scmi_mtk_tinysys_protocol_init,
	.ops = &mtk_tinysys_proto_ops,
	.events = &scmi_mtk_tinysys_protocol_events,
	.vendor_id = SCMI_MTK_VENDOR,
};
module_scmi_protocol(scmi_mtk_tinysys);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_ALIAS("scmi-protocol-" __stringify(SCMI_PROTOCOL_MTK_TINYSYS) "-" SCMI_MTK_VENDOR);
MODULE_DESCRIPTION("MediaTek SCMI TinySYS driver");
MODULE_LICENSE("GPL");
