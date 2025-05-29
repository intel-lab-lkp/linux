// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Generic netlink for energy model.
 *
 * Copyright (c) 2025 Valve Corporation.
 * Author: Changwoo Min <changwoo@igalia.com>
 */

#define pr_fmt(fmt) "energy_model: " fmt

#include <linux/energy_model.h>
#include <net/sock.h>
#include <net/genetlink.h>
#include <uapi/linux/energy_model.h>

#include "em_netlink.h"

static const struct genl_multicast_group em_genl_mcgrps[] = {
	[EM_GENL_EVENT_GROUP]  = { .name = EM_GENL_EVENT_GROUP_NAME,  },
};

static const struct nla_policy em_genl_policy[EM_GENL_ATTR_MAX + 1] = {
};

struct param {
	struct nlattr **attrs;
	struct sk_buff *msg;
};

typedef int (*cb_t)(struct param *);

static struct genl_family em_genl_family;

/*************************** Command encoding ********************************/

static int em_genl_cmd_pd_get_id(struct param *p)
{
	return -ENOTSUPP;
}

static int em_genl_cmd_pd_get_tbl(struct param *p)
{
	return -ENOTSUPP;
}

static const cb_t cmd_cb[] = {
	[EM_GENL_CMD_PD_GET_ID]			= em_genl_cmd_pd_get_id,
	[EM_GENL_CMD_PD_GET_TBL]		= em_genl_cmd_pd_get_tbl,
};

static int em_genl_cmd_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct param p = { .attrs = info->attrs };
	struct sk_buff *msg;
	void *hdr;
	int cmd = info->genlhdr->cmd;
	int ret = -EMSGSIZE;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	p.msg = msg;

	hdr = genlmsg_put_reply(msg, info, &em_genl_family, 0, cmd);
	if (!hdr)
		goto out_free_msg;

	ret = cmd_cb[cmd](&p);
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

static const struct genl_small_ops em_genl_ops[] = {
	{
		.cmd = EM_GENL_CMD_PD_GET_ID,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = em_genl_cmd_doit,
	},
	{
		.cmd = EM_GENL_CMD_PD_GET_TBL,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = em_genl_cmd_doit,
	},
};

static struct genl_family em_genl_family __ro_after_init = {
	.hdrsize	= 0,
	.name		= EM_GENL_FAMILY_NAME,
	.version	= EM_GENL_VERSION,
	.maxattr	= EM_GENL_ATTR_MAX,
	.policy		= em_genl_policy,
	.small_ops	= em_genl_ops,
	.n_small_ops	= ARRAY_SIZE(em_genl_ops),
	.resv_start_op	= __EM_GENL_CMD_MAX,
	.mcgrps		= em_genl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(em_genl_mcgrps),
};

int __init em_netlink_init(void)
{
	return genl_register_family(&em_genl_family);
}

void __init em_netlink_exit(void)
{
	genl_unregister_family(&em_genl_family);
}

