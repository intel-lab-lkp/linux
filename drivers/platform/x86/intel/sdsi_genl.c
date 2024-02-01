// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Intel Corporation.
 * All Rights Reserved.
 *
 * Author: David E. Box <david.e.box@linux.intel.com>
 *
 * Netlink ABI for Intel On Demand SPDM transport
 */
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/intel-sdsi.h>
#include <net/genetlink.h>

#include "sdsi.h"

static struct genl_family sdsi_nl_family;

static const struct nla_policy sdsi_genl_policy[SDSI_GENL_ATTR_MAX + 1] = {
	[SDSI_GENL_ATTR_DEVS]			= { .type = NLA_NESTED },
	[SDSI_GENL_ATTR_DEV_ID]			= { .type = NLA_U32 },
	[SDSI_GENL_ATTR_DEV_NAME]		= { .type = NLA_STRING },
	[SDSI_GENL_ATTR_SPDM_REQ]		= { .type = NLA_BINARY },
	[SDSI_GENL_ATTR_SPDM_RSP]		= { .type = NLA_BINARY },
	[SDSI_GENL_ATTR_SPDM_REQ_SIZE]		= { .type = NLA_U32 },
	[SDSI_GENL_ATTR_SPDM_RSP_SIZE]		= { .type = NLA_U32 },
};

struct param {
	struct nlattr **attrs;
	struct sk_buff *msg;
	struct sdsi_priv *priv;
};

typedef int (*sdsi_genl_cb_t)(struct param *);

static int sdsi_genl_cmd_spdm(struct param *p)
{
	struct sk_buff *msg = p->msg;
	struct sdsi_priv *priv = p->priv;
	void *response __free(kfree) = NULL;
	void *request;
	int rsp_size, req_size;
	int ret;

	if (!sdsi_supports_attestation(priv))
		return -EOPNOTSUPP;

	if (!p->attrs[SDSI_GENL_ATTR_SPDM_REQ])
		return -EINVAL;

	request = nla_data(p->attrs[SDSI_GENL_ATTR_SPDM_REQ]);
	req_size = nla_len(p->attrs[SDSI_GENL_ATTR_SPDM_REQ]);

	response = kmalloc(SDSI_SIZE_READ_MSG, GFP_KERNEL);
	if (!response)
		return -ENOMEM;

	rsp_size = sdsi_spdm_exchange(priv, request, req_size, response,
				      SDSI_SIZE_READ_MSG);
	if (rsp_size < 0)
		return rsp_size;

	ret = nla_put_u32(msg, SDSI_GENL_ATTR_DEV_ID, priv->id);
	if (ret)
		return ret;

	return nla_put(msg, SDSI_GENL_ATTR_SPDM_RSP, rsp_size,
		       response);
}

static int sdsi_genl_cmd_get_devs(struct param *p)
{
	struct sk_buff *msg = p->msg;
	struct nlattr *nest_start;
	struct sdsi_priv *priv = p->priv;

	nest_start = nla_nest_start(msg, SDSI_GENL_ATTR_DEVS);
	if (!nest_start)
		return -EMSGSIZE;

	if (nla_put_u32(msg, SDSI_GENL_ATTR_DEV_ID, priv->id) ||
	    nla_put_string(msg, SDSI_GENL_ATTR_DEV_NAME, dev_name(priv->dev)))
		goto out_cancel_nest;

	nla_nest_end(msg, nest_start);

	return 0;

out_cancel_nest:
	nla_nest_cancel(msg, nest_start);

	return -EMSGSIZE;
}

static int sdsi_genl_cmd_get_info(struct param *p)
{
	struct sk_buff *msg = p->msg;
	int ret;

	ret = nla_put_u32(msg, SDSI_GENL_ATTR_SPDM_REQ_SIZE,
			  SDSI_SIZE_WRITE_MSG - (2 * sizeof(u64)));
	if (ret)
		return ret;

	return nla_put_u32(msg, SDSI_GENL_ATTR_SPDM_RSP_SIZE,
			   SDSI_SIZE_READ_MSG - (sizeof(u64)));
}

static sdsi_genl_cb_t sdsi_genl_cmd_cb[] = {
	[SDSI_GENL_CMD_GET_DEVS]		= sdsi_genl_cmd_get_devs,
	[SDSI_GENL_CMD_GET_INFO]		= sdsi_genl_cmd_get_info,
	[SDSI_GENL_CMD_GET_SPDM]		= sdsi_genl_cmd_spdm,
};

static int sdsi_genl_cmd_dumpit(struct sk_buff *skb,
				struct netlink_callback *cb)
{
	struct param p = { .msg = skb };
	struct sdsi_priv *entry;
	const struct genl_dumpit_info *info = genl_dumpit_info(cb);
	int cmd = info->op.cmd;
	int ret = 0, idx = 0;
	void *hdr;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &sdsi_nl_family, NLM_F_MULTI, cmd);
	if (!hdr)
		return -EMSGSIZE;

	mutex_lock(&sdsi_list_lock);
	list_for_each_entry(entry, &sdsi_list, node) {
		p.priv = entry;
		ret = sdsi_genl_cmd_cb[cmd](&p);
		if (ret)
			break;
		idx++;
	}
	mutex_unlock(&sdsi_list_lock);

	if (ret)
		goto out_cancel_msg;

	genlmsg_end(skb, hdr);

	return 0;

out_cancel_msg:
	genlmsg_cancel(skb, hdr);
	return ret;
}

static int sdsi_genl_cmd_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct param p = { .attrs = info->attrs };
	struct sdsi_priv *priv, *entry;
	struct sk_buff *msg;
	void *hdr;
	int cmd = info->genlhdr->cmd;
	int ret = 0;
	int id;

	if (!p.attrs[SDSI_GENL_ATTR_DEV_ID])
		return -EINVAL;

	id = nla_get_u32(p.attrs[SDSI_GENL_ATTR_DEV_ID]);

	priv = sdsi_dev_get_by_id(id);
	if (!priv)
		return -ENODEV;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	p.msg = msg;
	p.priv = priv;

	hdr = genlmsg_put_reply(msg, info, &sdsi_nl_family, 0, cmd);
	if (!hdr)
		goto out_free_msg;

	mutex_lock(&sdsi_list_lock);
	list_for_each_entry(entry, &sdsi_list, node) {
		if (entry == priv) {
			ret = sdsi_genl_cmd_cb[cmd](&p);
			if (ret)
				break;
			break;
		}
	}
	mutex_unlock(&sdsi_list_lock);

	if (ret)
		goto out_cancel_msg;

	genlmsg_end(msg, hdr);

	return genlmsg_reply(msg, info);

out_cancel_msg:
	genlmsg_cancel(msg, hdr);
out_free_msg:
	nlmsg_free(msg);

	return ret;
}

static const struct genl_ops sdsi_genl_ops[] = {
	{
		.cmd = SDSI_GENL_CMD_GET_DEVS,
		.doit = sdsi_genl_cmd_doit,
		.dumpit = sdsi_genl_cmd_dumpit,
	},
	{
		.cmd = SDSI_GENL_CMD_GET_INFO,
		.doit = sdsi_genl_cmd_doit,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = SDSI_GENL_CMD_GET_SPDM,
		.doit = sdsi_genl_cmd_doit,
		.flags = GENL_ADMIN_PERM,
	},
};

static struct genl_family sdsi_nl_family __ro_after_init = {
	.hdrsize	= 0,
	.name		= SDSI_FAMILY_NAME,
	.version	= SDSI_FAMILY_VERSION,
	.maxattr	= SDSI_GENL_ATTR_MAX,
	.policy		= sdsi_genl_policy,
	.ops		= sdsi_genl_ops,
	.resv_start_op	= SDSI_GENL_CMD_MAX + 1,
	.n_ops		= ARRAY_SIZE(sdsi_genl_ops),
};

int __init sdsi_netlink_init(void)
{
	return genl_register_family(&sdsi_nl_family);
}

int sdsi_netlink_exit(void)
{
	return genl_unregister_family(&sdsi_nl_family);
}
