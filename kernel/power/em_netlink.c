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
	/* Performance domain */
	[EM_GENL_ATTR_PD]			= { .type = NLA_NESTED },
	/* Performance table of a performance domain */
	[EM_GENL_ATTR_PD_TBL]			= { .type = NLA_NESTED },
};

struct param {
	struct nlattr **attrs;
	struct sk_buff *msg;
	int pd_id;
};

typedef int (*cb_t)(struct param *);

static struct genl_family em_genl_family;

/**************************** Event encoding *********************************/
static int __em_genl_event_pd_id(struct param *p)
{
	if (nla_put_u32(p->msg, EM_PD_GENL_ATTR_ID, p->pd_id))
		return -EMSGSIZE;

	return 0;
}

static int em_genl_event_pd_create(struct param *p)
{
	return __em_genl_event_pd_id(p);
}

static int em_genl_event_pd_delete(struct param *p)
{
	return __em_genl_event_pd_id(p);
}

static int em_genl_event_pd_update(struct param *p)
{
	return __em_genl_event_pd_id(p);
}

static const cb_t event_cb[] = {
	[EM_GENL_EVENT_PD_CREATE] = em_genl_event_pd_create,
	[EM_GENL_EVENT_PD_DELETE] = em_genl_event_pd_delete,
	[EM_GENL_EVENT_PD_UPDATE] = em_genl_event_pd_update,
};

static int em_genl_send_event(enum em_genl_event event, struct param *p)
{
	struct sk_buff *msg;
	int ret = -EMSGSIZE;
	void *hdr;

	if (!genl_has_listeners(&em_genl_family, &init_net, EM_GENL_EVENT_GROUP))
		return 0;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	p->msg = msg;

	hdr = genlmsg_put(msg, 0, 0, &em_genl_family, 0, event);
	if (!hdr)
		goto out_free_msg;

	ret = event_cb[event](p);
	if (ret)
		goto out_cancel_msg;

	genlmsg_end(msg, hdr);

	genlmsg_multicast(&em_genl_family, msg, 0, EM_GENL_EVENT_GROUP, GFP_KERNEL);

	return 0;

out_cancel_msg:
	genlmsg_cancel(msg, hdr);
out_free_msg:
	nlmsg_free(msg);

	return ret;
}

int em_notify_pd_create(const struct em_perf_domain *pd)
{
	struct param p = { .pd_id = pd->id };

	return em_genl_send_event(EM_GENL_EVENT_PD_CREATE, &p);
}


int em_notify_pd_delete(const struct em_perf_domain *pd)
{
	struct param p = { .pd_id = pd->id };

	return em_genl_send_event(EM_GENL_EVENT_PD_DELETE, &p);
}

int em_notify_pd_update(const struct em_perf_domain *pd)
{
	struct param p = { .pd_id = pd->id };

	return em_genl_send_event(EM_GENL_EVENT_PD_UPDATE, &p);
}

/*************************** Command encoding ********************************/

static int __em_genl_cmd_pd_get_id(struct em_perf_domain *pd, void *data)
{
	char cpus_buf[EM_PD_CPUS_LENGTH];
	struct sk_buff *msg = data;
	struct nlattr *entry;

	entry = nla_nest_start(msg, EM_PD_ENTRY_GENL_ATTR_PD);
	if (!entry)
		goto out_cancel_nest;

	if (nla_put_u32(msg, EM_PD_GENL_ATTR_ID, pd->id))
		goto out_cancel_nest;

	if (nla_put_u64_64bit(msg, EM_PD_GENL_ATTR_FLAGS, pd->flags,
			      EM_PD_GENL_ATTR_PAD))
		goto out_cancel_nest;

	snprintf(cpus_buf, sizeof(cpus_buf), "%*pb",
		 cpumask_pr_args(to_cpumask(pd->cpus)));
	if (nla_put_string(msg, EM_PD_GENL_ATTR_CPUS, cpus_buf))
		goto out_cancel_nest;

	nla_nest_end(msg, entry);

	return 0;

out_cancel_nest:
	nla_nest_cancel(msg, entry);

	return -EMSGSIZE;
}

static int em_genl_cmd_pd_get_id(struct param *p)
{
	struct sk_buff *msg = p->msg;
	struct nlattr *start_pd;
	int ret;

	start_pd = nla_nest_start(msg, EM_GENL_ATTR_PD);
	if (!start_pd)
		return -EMSGSIZE;

	ret = for_each_em_perf_domain(__em_genl_cmd_pd_get_id, msg);
	if (ret)
		goto out_cancel_nest;

	nla_nest_end(msg, start_pd);

	return 0;

out_cancel_nest:
	nla_nest_cancel(msg, start_pd);

	return ret;
}

static int em_genl_cmd_pd_get_tbl(struct param *p)
{
	struct sk_buff *msg = p->msg;
	struct em_perf_domain *pd;
	struct em_perf_state *table, *ps;
	struct nlattr *start_tbl, *entry;
	int id, i;

	if (!p->attrs[EM_PD_GENL_ATTR_ID])
		return -EINVAL;

	id = nla_get_u32(p->attrs[EM_PD_GENL_ATTR_ID]);

	pd = em_perf_domain_get_by_id(id);
	if (!pd)
		return -EINVAL;

	start_tbl = nla_nest_start(msg, EM_GENL_ATTR_PD_TBL);
	if (!start_tbl )
		return -EMSGSIZE;

	rcu_read_lock();
	table = em_perf_state_from_pd(pd);

	for (i = 0; i < pd->nr_perf_states; i++) {
		ps = &table[i];

		entry = nla_nest_start(msg, EM_TBL_ENTRY_GENL_ATTR_PD);
		if (!entry)
			goto out_cancel_nest;

		if (nla_put_u64_64bit(msg, EM_TBL_GENL_ATTR_PS_PERFORMANCE,
				      ps->performance, EM_TBL_GENL_ATTR_PAD))
			goto out_cancel_nest2;
		if (nla_put_u64_64bit(msg, EM_TBL_GENL_ATTR_PS_FREQUENCY,
				      ps->frequency, EM_TBL_GENL_ATTR_PAD))
			goto out_cancel_nest2;
		if (nla_put_u64_64bit(msg, EM_TBL_GENL_ATTR_PS_POWER,
				      ps->power, EM_TBL_GENL_ATTR_PAD))
			goto out_cancel_nest2;
		if (nla_put_u64_64bit(msg, EM_TBL_GENL_ATTR_PS_COST,
				      ps->cost, EM_TBL_GENL_ATTR_PAD))
			goto out_cancel_nest2;
		if (nla_put_u64_64bit(msg, EM_TBL_GENL_ATTR_PS_FLAGS,
				      ps->flags, EM_TBL_GENL_ATTR_PAD))
			goto out_cancel_nest2;

		nla_nest_end(msg, entry);
	}
	rcu_read_unlock();

	nla_nest_end(msg, start_tbl);

	return 0;

out_cancel_nest2:
	nla_nest_cancel(msg, entry);

out_cancel_nest:
	rcu_read_unlock();

	nla_nest_cancel(msg, start_tbl);
	return -EMSGSIZE;
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

