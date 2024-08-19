// SPDX-License-Identifier: GPL-2.0-only

#include <net/genetlink.h>
#include <net/sock.h>

#include "dlm_internal.h"
#include "lockspace.h"
#include "midcomms.h"
#include "config.h"
#include "member.h"
#include "lock.h"

#include "nldlm.h"

/* nldlm netlink family */
static struct genl_family nldlm_genl_family;

enum nldlm_multicast_groups {
	NLDLM_MCGRP_CONFIG,
	NLDLM_MCGRP_EVENT,
};

static const struct genl_multicast_group nldlm_mcgrps[] = {
	[NLDLM_MCGRP_CONFIG] = { .name = "config", },
	[NLDLM_MCGRP_EVENT] = { .name = "event", },
};

static int nldlm_put_ls(struct sk_buff *skb, const char *lsname,
			int attrtype)
{
	struct nlattr *nl_ls;
	int rv;

	nl_ls = nla_nest_start(skb, attrtype);
	if (!nl_ls)
		return -ENOBUFS;

	rv = nla_put_string(skb, NLDLM_LS_ATTR_NAME, lsname);
	if (rv < 0) {
		nla_nest_cancel(skb, nl_ls);
		return rv;
	}

	nla_nest_end(skb, nl_ls);
	return 0;
}

static int nldlm_put_ls_event(struct sk_buff *skb, const struct dlm_ls *ls,
			      u32 portid, u32 seq, int flags, uint32_t cmd,
			      uint32_t type)
{
	struct nlattr *nl_event;
	void *hdr;
	int rv;

	hdr = genlmsg_put(skb, 0, 0, &nldlm_genl_family, 0, cmd);
	if (!hdr)
		return -ENOBUFS;

	nl_event = nla_nest_start(skb, NLDLM_ATTR_LS_EVENT);
	if (!nl_event)
		return -ENOBUFS;

	rv = nldlm_put_ls(skb, ls->ls_name, NLDLM_LS_EVENT_ATTR_LS);
	if (rv < 0) {
		nla_nest_cancel(skb, nl_event);
		goto err;
	}

	rv = nla_put_u32(skb, NLDLM_LS_EVENT_ATTR_TYPE, type);
	if (rv) {
		nla_nest_cancel(skb, nl_event);
		goto err;
	}

	nla_nest_end(skb, nl_event);
	genlmsg_end(skb, hdr);
	return 0;

err:
	genlmsg_cancel(skb, hdr);
	return -ENOBUFS;
}

int nldlm_ls_event(const struct dlm_ls *ls, uint32_t type)
{
	struct sk_buff *skb;
	int rv;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	rv = nldlm_put_ls_event(skb, ls, 0, 0, 0, NLDLM_CMD_LS_EVENT, type);
	if (rv < 0) {
		nlmsg_free(skb);
		return rv;
	}

	return genlmsg_multicast_netns(&nldlm_genl_family, read_pnet(&ls->ls_dn->net), skb, 0,
				       NLDLM_MCGRP_EVENT, GFP_ATOMIC);
}

static const struct nla_policy nldlm_ls_policy[NLDLM_LS_ATTR_MAX + 1] = {
	[NLDLM_LS_ATTR_NAME] = { .type = NLA_NUL_STRING },
};

static int nldlm_parse_ls(const struct nlattr *nla, char *lsname)
{
	struct nlattr *ls_attrs[NLDLM_LS_ATTR_MAX + 1];

	if (!nla)
		return -EINVAL;

	if (nla_parse_nested(ls_attrs, NLDLM_LS_ATTR_MAX, nla,
			     nldlm_ls_policy, NULL))
		return -EINVAL;

	if (lsname) {
		if (!ls_attrs[NLDLM_LS_ATTR_NAME])
			return -EINVAL;

		nla_strscpy(lsname, ls_attrs[NLDLM_LS_ATTR_NAME],
			    DLM_LOCKSPACE_LEN);
	}

	return 0;
}

static int __nldlm_get_ls(struct sk_buff *skb, struct dlm_cfg_ls *ls,
			  u32 portid, u32 seq, struct netlink_callback *cb,
			  int flags)
{
	void *hdr;
	int rv;

	hdr = genlmsg_put(skb, portid, seq, &nldlm_genl_family, flags,
			  NLDLM_CMD_GET_LS);
	if (!hdr)
		return -EMSGSIZE;

	if (cb)
		genl_dump_check_consistent(cb, hdr);

	rv = nldlm_put_ls(skb, ls->name, NLDLM_ATTR_LS);
	if (rv < 0)
		goto err;

	genlmsg_end(skb, hdr);
	return 0;

err:
	genlmsg_cancel(skb, hdr);
	return rv;
}

static int nldlm_dump_ls(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	unsigned int idx = cb->args[0];
	struct dlm_cfg_ls *ls;
	int rv;

	mutex_lock(&dn->cfg_lock);
	list_for_each_entry(ls, &dn->lockspaces, list) {
		if (ls->idx < idx)
			continue;

		rv = __nldlm_get_ls(skb, ls, NETLINK_CB(cb->skb).portid,
				    cb->nlh->nlmsg_seq, cb, NLM_F_MULTI);
		if (rv < 0)
			break;

		idx = ls->idx + 1;
	}
	mutex_unlock(&dn->cfg_lock);

	cb->args[0] = idx;
	return skb->len;
}

static int nldlm_get_ls(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct dlm_cfg_ls *ls, *ls_iter = NULL;
	char lsname[DLM_LOCKSPACE_LEN];
	struct sk_buff *skb;
	int rv;

	rv = nldlm_parse_ls(info->attrs[NLDLM_ATTR_LS], lsname);
	if (rv < 0)
		return rv;

	mutex_lock(&dn->cfg_lock);
	list_for_each_entry(ls_iter, &dn->lockspaces, list) {
		if (!strncmp(ls_iter->name, lsname, DLM_LOCKSPACE_LEN)) {
			ls = ls_iter;
			break;
		}
	}

	if (!ls) {
		rv = -ENOENT;
		goto err;
	}

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		rv = -ENOMEM;
		goto err;
	}

	rv = __nldlm_get_ls(skb, ls, info->snd_portid,
			    info->snd_seq, NULL, 0);
	if (rv < 0) {
		nlmsg_free(skb);
		goto err;
	}

	rv = genlmsg_reply(skb, info);

err:
	mutex_unlock(&dn->cfg_lock);
	return rv;
}

static int nldlm_put_addr(struct sk_buff *skb, const struct sockaddr_storage *addr,
			  int attrtype)
{
	struct nlattr *nl_addr;
	int rv;

	nl_addr = nla_nest_start(skb, attrtype);
	if (!nl_addr)
		return -ENOBUFS;

	rv = nla_put_u16(skb, NLDLM_ADDR_ATTR_FAMILY, addr->ss_family);
	if (rv) {
		nla_nest_cancel(skb, nl_addr);
		return -ENOBUFS;
	}

	switch (addr->ss_family) {
	case AF_INET:
		rv = nla_put_in_addr(skb, NLDLM_ADDR_ATTR_ADDR4,
				     ((struct sockaddr_in *)addr)->sin_addr.s_addr);
		if (rv) {
			nla_nest_cancel(skb, nl_addr);
			return -ENOBUFS;
		}
		break;
	case AF_INET6:
		rv = nla_put_in6_addr(skb, NLDLM_ADDR_ATTR_ADDR6,
				      &((struct sockaddr_in6 *)addr)->sin6_addr);
		if (rv) {
			nla_nest_cancel(skb, nl_addr);
			return -ENOBUFS;
		}
		break;
	default:
		nla_nest_cancel(skb, nl_addr);
		return -EINVAL;
	}

	nla_nest_end(skb, nl_addr);
	return 0;
}

static int nldlm_put_node(struct sk_buff *skb, struct dlm_cfg_node *nd,
			  int attrtype)
{
	struct nlattr *nl_nd, *nl_addrs;
	int rv, i;

	nl_nd = nla_nest_start(skb, attrtype);
	if (!nl_nd)
		return -ENOBUFS;

	rv = nla_put_le32(skb, NLDLM_NODE_ATTR_ID, cpu_to_le32(nd->id));
	if (rv < 0) {
		nla_nest_cancel(skb, nl_nd);
		return rv;
	}

	nl_addrs = nla_nest_start(skb, NLDLM_NODE_ATTR_ADDRS);
	if (!nl_addrs) {
		nla_nest_cancel(skb, nl_nd);
		return -ENOBUFS;
	}

	for (i = 0; i < nd->addrs_count; i++) {
		rv = nldlm_put_addr(skb, &nd->addrs[i], i + 1);
		if (rv) {
			nla_nest_cancel(skb, nl_addrs);
			nla_nest_cancel(skb, nl_nd);
			return rv;
		}
	}

	nla_nest_end(skb, nl_addrs);
	nla_nest_end(skb, nl_nd);
	return 0;
}

static int __nldlm_get_node(struct sk_buff *skb, struct dlm_cfg_node *nd,
			    u32 portid, u32 seq,
			    struct netlink_callback *cb, int flags)
{
	void *hdr;
	int rv;

	hdr = genlmsg_put(skb, portid, seq, &nldlm_genl_family, flags,
			  NLDLM_CMD_GET_NODE);
	if (!hdr)
		return -EMSGSIZE;

	if (cb)
		genl_dump_check_consistent(cb, hdr);

	rv = nldlm_put_node(skb, nd, NLDLM_ATTR_NODE);
	if (rv < 0)
		goto err;

	genlmsg_end(skb, hdr);
	return 0;

err:
	genlmsg_cancel(skb, hdr);
	return rv;
}

static const struct nla_policy
nldlm_addr_policy[NLDLM_ADDR_ATTR_MAX + 1] = {
	[NLDLM_ADDR_ATTR_FAMILY] = { .type = NLA_U16 },
	[NLDLM_ADDR_ATTR_ADDR4] = { .type = NLA_BE32 },
	[NLDLM_ADDR_ATTR_ADDR6] = { .type = NLA_BINARY,
				    .len = sizeof(struct in6_addr)},
};

static int nldlm_parse_addr(const struct nlattr *nla, struct sockaddr_storage *addr)
{
	struct nlattr *addr_attrs[NLDLM_ADDR_ATTR_MAX + 1];

	if (nla_parse_nested(addr_attrs, NLDLM_ADDR_ATTR_MAX, nla,
			     nldlm_addr_policy, NULL))
		return -EINVAL;

	if (!addr_attrs[NLDLM_ADDR_ATTR_FAMILY])
		return -EINVAL;

	addr->ss_family = nla_get_u16(addr_attrs[NLDLM_ADDR_ATTR_FAMILY]);
	switch (addr->ss_family) {
	case AF_INET:
		if (!addr_attrs[NLDLM_ADDR_ATTR_ADDR4])
			return -EINVAL;

		((struct sockaddr_in *)addr)->sin_addr.s_addr =
			nla_get_in_addr(addr_attrs[NLDLM_ADDR_ATTR_ADDR4]);
		break;
	case AF_INET6:
		if (!addr_attrs[NLDLM_ADDR_ATTR_ADDR6])
			return -EINVAL;

		((struct sockaddr_in6 *)addr)->sin6_addr =
			nla_get_in6_addr(addr_attrs[NLDLM_ADDR_ATTR_ADDR6]);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct nla_policy
nldlm_node_policy[NLDLM_NODE_ATTR_MAX + 1] = {
	[NLDLM_NODE_ATTR_ID] = { .type = NLA_U32 },
	[NLDLM_NODE_ATTR_MARK] = { .type = NLA_U32 },
	[NLDLM_NODE_ATTR_ADDRS] = { .type = NLA_NESTED },
};

static int nldlm_parse_node(const struct nlattr *nla, __le32 *nodeid,
			    struct sockaddr_storage *addrs, size_t *addrs_count,
			    uint32_t *mark)
{
	struct nlattr *nd_attrs[NLDLM_NODE_ATTR_MAX + 1];
	int rem, rv;

	if (!nla)
		return -EINVAL;

	if (nla_parse_nested(nd_attrs, NLDLM_NODE_ATTR_MAX, nla,
			     nldlm_node_policy, NULL))
		return -EINVAL;

	if (nodeid) {
		if (!nd_attrs[NLDLM_NODE_ATTR_ID])
			return -EINVAL;

		*nodeid = nla_get_le32(nd_attrs[NLDLM_NODE_ATTR_ID]);
	}

	if (addrs && addrs_count) {
		if (!nd_attrs[NLDLM_NODE_ATTR_ADDRS])
			return -EINVAL;

		*addrs_count = 0;
		nla_for_each_nested(nla, nd_attrs[NLDLM_NODE_ATTR_ADDRS], rem) {
			if (*addrs_count == DLM_MAX_ADDR_COUNT)
				return -ENOSPC;

			rv = nldlm_parse_addr(nla, &addrs[*addrs_count]);
			if (rv)
				return rv;

			(*addrs_count)++;
		}
	}

	if (mark) {
		if (nd_attrs[NLDLM_NODE_ATTR_MARK])
			*mark = nla_get_u32(nd_attrs[NLDLM_NODE_ATTR_MARK]);
		else
			*mark = DLM_DEFAULT_MARK;
	}

	return 0;
}

static int nldlm_get_node(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct dlm_cfg_node *nd;
	struct sk_buff *skb;
	__le32 nodeid;
	int rv;

	rv = nldlm_parse_node(info->attrs[NLDLM_ATTR_NODE], &nodeid, NULL, NULL,
			      NULL);
	if (rv)
		return rv;

	mutex_lock(&dn->cfg_lock);
	nd = dlm_cfg_get_node(dn, le32_to_cpu(nodeid));
	if (!nd) {
		rv = -ENOENT;
		goto out;
	}

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		rv = -ENOMEM;
		goto out;
	}

	rv = __nldlm_get_node(skb, nd, info->snd_portid,
			      info->snd_seq, NULL, 0);
	if (rv < 0) {
		nlmsg_free(skb);
		goto out;
	}

	rv = genlmsg_reply(skb, info);

out:
	mutex_unlock(&dn->cfg_lock);
	return rv;
}

static int nldlm_dump_nodes(struct sk_buff *skb,
			    struct netlink_callback *cb)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	unsigned int idx = cb->args[0];
	struct dlm_cfg_node *nd;
	int rv;

	mutex_lock(&dn->cfg_lock);
	list_for_each_entry(nd, &dn->nodes, list) {
		if (nd->idx < idx)
			continue;

		rv = __nldlm_get_node(skb, nd, NETLINK_CB(cb->skb).portid,
				      cb->nlh->nlmsg_seq, cb, NLM_F_MULTI);
		if (rv < 0)
			break;

		idx = nd->idx + 1;
	}
	mutex_unlock(&dn->cfg_lock);

	cb->args[0] = idx;
	return skb->len;
}

static int nldlm_new_node(struct sk_buff *msg, struct genl_info *info)
{
	struct sockaddr_storage addrs[DLM_MAX_ADDR_COUNT] = {};
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	size_t addrs_count;
	uint32_t mark;
	__le32 nodeid;
	int rv;

	rv = nldlm_parse_node(info->attrs[NLDLM_ATTR_NODE], &nodeid,
			      addrs, &addrs_count, &mark);
	if (rv < 0)
		return rv;

	return dlm_cfg_new_node(dn, le32_to_cpu(nodeid), mark, addrs,
				addrs_count);
}

static int nldlm_del_node(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	__le32 nodeid;
	int rv;

	rv = nldlm_parse_node(info->attrs[NLDLM_ATTR_NODE], &nodeid, NULL,
			      NULL, NULL);
	if (rv < 0)
		return rv;

	return dlm_cfg_del_node(dn, le32_to_cpu(nodeid));
}

static int nldlm_put_member(struct sk_buff *skb,
			    const struct dlm_cfg_member *mb, int attrtype)
{
	struct nlattr *nl_mb;
	int rv;

	nl_mb = nla_nest_start(skb, NLDLM_ATTR_LS_MEMBER);
	if (!nl_mb)
		return -ENOBUFS;

	rv = nldlm_put_ls(skb, mb->ls->name, NLDLM_LS_MEMBER_ATTR_LS);
	if (rv < 0) {
		nla_nest_cancel(skb, nl_mb);
		return rv;
	}

	rv = nldlm_put_node(skb, mb->nd, NLDLM_LS_MEMBER_ATTR_NODE);
	if (rv < 0) {
		nla_nest_cancel(skb, nl_mb);
		return rv;
	}

	rv = nla_put_u32(skb, NLDLM_LS_MEMBER_ATTR_WEIGHT, mb->weight);
	if (rv < 0) {
		nla_nest_cancel(skb, nl_mb);
		return rv;
	}

	nla_nest_end(skb, nl_mb);
	return 0;
}

static int __nldlm_get_member(struct sk_buff *skb,
			      const struct dlm_cfg_member *mb,
			      u32 portid, u32 seq,
			      struct netlink_callback *cb, int flags)
{
	void *hdr;
	int rv;

	hdr = genlmsg_put(skb, portid, seq, &nldlm_genl_family, flags,
			  NLDLM_CMD_GET_LS_MEMBER);
	if (!hdr)
		return -EMSGSIZE;

	if (cb)
		genl_dump_check_consistent(cb, hdr);

	rv = nldlm_put_member(skb, mb, NLDLM_ATTR_LS_MEMBER);
	if (rv < 0)
		goto err;

	genlmsg_end(skb, hdr);
	return 0;

err:
	genlmsg_cancel(skb, hdr);
	return rv;
}

static const struct nla_policy nldlm_member_policy[NLDLM_LS_MEMBER_ATTR_MAX + 1] = {
	[NLDLM_LS_MEMBER_ATTR_LS] = { .type = NLA_NESTED },
	[NLDLM_LS_MEMBER_ATTR_NODE] = { .type = NLA_NESTED },
	[NLDLM_LS_MEMBER_ATTR_WEIGHT] = { .type = NLA_U32 },
};

static int nldlm_parse_member(const struct nlattr *nla, char *lsname,
			      __le32 *nodeid, uint32_t *weight)
{
	struct nlattr *member_attrs[NLDLM_LS_MEMBER_ATTR_MAX + 1];
	int rv;

	if (!nla)
		return -EINVAL;

	if (nla_parse_nested(member_attrs, NLDLM_LS_MEMBER_ATTR_MAX, nla,
			     nldlm_member_policy, NULL))
		return -EINVAL;

	if (lsname) {
		rv = nldlm_parse_ls(member_attrs[NLDLM_LS_MEMBER_ATTR_LS],
				    lsname);
		if (rv)
			return rv;
	}

	if (nodeid) {
		rv = nldlm_parse_node(member_attrs[NLDLM_LS_MEMBER_ATTR_NODE],
				      nodeid, NULL, NULL, NULL);
		if (rv)
			return rv;
	}

	if (weight) {
		if (member_attrs[NLDLM_LS_MEMBER_ATTR_WEIGHT])
			*weight = nla_get_u32(member_attrs[NLDLM_LS_MEMBER_ATTR_WEIGHT]);
		else
			*weight = DLM_DEFAULT_WEIGHT;
	}

	return 0;
}

static int nldlm_get_member(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_cfg_member *mb;
	struct sk_buff *skb;
	__le32 nodeid;
	int rv;

	rv = nldlm_parse_member(info->attrs[NLDLM_ATTR_LS_MEMBER], lsname,
				&nodeid, NULL);
	if (rv < 0)
		return rv;

	mutex_lock(&dn->cfg_lock);
	mb = dlm_cfg_get_ls_member(dn, lsname, le32_to_cpu(nodeid));
	if (!mb) {
		rv = -ENOENT;
		goto out;
	}

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		rv = -ENOMEM;
		goto out;
	}

	rv = __nldlm_get_member(skb, mb, info->snd_portid,
				info->snd_seq, NULL, 0);
	if (rv < 0) {
		nlmsg_free(skb);
		goto out;
	}

	rv = genlmsg_reply(skb, info);

out:
	mutex_unlock(&dn->cfg_lock);
	return rv;
}

static int nldlm_dump_members(struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	unsigned int idx = cb->args[0];
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_cfg_member *mb;
	struct dlm_cfg_ls *ls;
	int rv;

	rv = nldlm_parse_ls(info->attrs[NLDLM_ATTR_LS], lsname);
	if (rv < 0)
		return rv;

	mutex_lock(&dn->cfg_lock);
	ls = dlm_cfg_get_ls(dn, lsname);
	if (!ls) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	list_for_each_entry(mb, &ls->members, list) {
		if (mb->idx < idx)
			continue;

		rv = __nldlm_get_member(skb, mb, NETLINK_CB(cb->skb).portid,
					cb->nlh->nlmsg_seq, cb, NLM_F_MULTI);
		if (rv < 0)
			break;

		idx = mb->idx + 1;
	}
	mutex_unlock(&dn->cfg_lock);

	cb->args[0] = idx;
	return skb->len;
}

static int nldlm_ls_member_add(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	uint32_t weight;
	__le32 nodeid;
	int rv;

	rv = nldlm_parse_member(info->attrs[NLDLM_ATTR_LS_MEMBER], lsname,
				&nodeid, &weight);
	if (rv < 0)
		return rv;

	return dlm_cfg_add_member(dn, lsname, le32_to_cpu(nodeid), weight);
}

static int nldlm_ls_member_del(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	__le32 nodeid;
	int rv;

	rv = nldlm_parse_member(info->attrs[NLDLM_ATTR_LS_MEMBER], lsname,
				&nodeid, NULL);
	if (rv < 0)
		return rv;

	return dlm_cfg_del_member(dn, lsname, le32_to_cpu(nodeid));
}

static const struct nla_policy
nldlm_ls_event_policy[NLDLM_LS_EVENT_ATTR_MAX + 1] = {
	[NLDLM_LS_EVENT_ATTR_LS] = { .type = NLA_NESTED },
	[NLDLM_LS_EVENT_ATTR_TYPE] = { .type = NLA_U32 },
	[NLDLM_LS_EVENT_ATTR_CTRL] = { .type = NLA_U32 },
	[NLDLM_LS_EVENT_ATTR_RESULT] = { .type = NLA_U32 },
};

static int nldlm_parse_ls_event(const struct nlattr *nla, char *lsname,
				uint32_t *type, uint32_t *ctrl,
				uint32_t *result)
{
	struct nlattr *ls_event_attrs[NLDLM_LS_EVENT_ATTR_MAX + 1];
	int rv;

	if (!nla)
		return -EINVAL;

	if (nla_parse_nested(ls_event_attrs, NLDLM_LS_EVENT_ATTR_MAX, nla,
			     nldlm_ls_event_policy, NULL))
		return -EINVAL;

	if (lsname) {
		rv = nldlm_parse_ls(ls_event_attrs[NLDLM_LS_EVENT_ATTR_LS],
				    lsname);
		if (rv < 0)
			return -EINVAL;
	}

	if (type) {
		if (!ls_event_attrs[NLDLM_LS_EVENT_ATTR_TYPE])
			return -EINVAL;

		*type = nla_get_u32(ls_event_attrs[NLDLM_LS_EVENT_ATTR_TYPE]);
	}

	if (ctrl) {
		if (!ls_event_attrs[NLDLM_LS_EVENT_ATTR_CTRL])
			return -EINVAL;

		*ctrl = nla_get_u32(ls_event_attrs[NLDLM_LS_EVENT_ATTR_CTRL]);
	}

	if (result) {
		if (!ls_event_attrs[NLDLM_LS_EVENT_ATTR_RESULT])
			return -EINVAL;

		*result = nla_get_u32(ls_event_attrs[NLDLM_LS_EVENT_ATTR_RESULT]);
	}

	return 0;
}

static int nldlm_ls_event_done(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_ls *ls;
	uint32_t result;
	int rv;

	rv = nldlm_parse_ls_event(info->attrs[NLDLM_ATTR_LS_EVENT],
				  lsname, NULL, NULL, &result);
	if (rv < 0)
		return rv;

	ls = dlm_find_lockspace_name(dn, lsname);
	if (!ls)
		return -ENOENT;

	switch (result) {
	case NLDLM_EVENT_MEMBER_SUCCESS:
		rv = 0;
		break;
	case NLDLM_EVENT_MEMBER_FAILURE:
		rv = -1;
		break;
	default:
		dlm_put_lockspace(ls);
		return -EINVAL;
	}

	ls->ls_uevent_result = rv;
	set_bit(LSFL_UEVENT_WAIT, &ls->ls_flags);
	wake_up(&ls->ls_uevent_wait);

	dlm_put_lockspace(ls);
	return 0;
}

static int nldlm_ls_control(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_ls *ls;
	uint32_t ctrl;
	int rv;

	rv = nldlm_parse_ls_event(info->attrs[NLDLM_ATTR_LS_EVENT],
				  lsname, NULL, &ctrl, NULL);
	if (rv < 0)
		return rv;

	ls = dlm_find_lockspace_name(dn, lsname);
	if (!ls)
		return -ENOENT;

	switch (ctrl) {
	case NLDLM_LS_CTRL_STOP:
		dlm_ls_stop(ls);
		break;
	case NLDLM_LS_CTRL_START:
		dlm_ls_start(ls);
		break;
	default:
		rv = -EINVAL;
		break;
	}

	dlm_put_lockspace(ls);
	return rv;
}

static int nldlm_get_config(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *nl_cfg;
	struct sk_buff *skb;
	uint32_t log_level;
	void *hdr;
	int rv;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, info->snd_portid, info->snd_seq,
			  &nldlm_genl_family, 0, NLDLM_CMD_GET_CONFIG);
	if (!hdr) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	nl_cfg = nla_nest_start(skb, NLDLM_ATTR_CFG);
	if (!nl_cfg) {
		nlmsg_free(skb);
		return -ENOBUFS;
	}

	mutex_lock(&dn->cfg_lock);
	if (dn->our_node) {
		rv = nldlm_put_node(skb, dn->our_node,
				    NLDLM_CFG_ATTR_OUR_NODE);
		if (rv < 0)
			goto err;
	}

	rv = nla_put_string(skb, NLDLM_CFG_ATTR_CLUSTER_NAME,
			    dn->config.ci_cluster_name);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(skb, NLDLM_CFG_ATTR_PROTOCOL,
			 dn->config.ci_protocol);
	if (rv < 0)
		goto err;

	rv = nla_put_be16(skb, NLDLM_CFG_ATTR_PORT,
			  dn->config.ci_tcp_port);
	if (rv < 0)
		goto err;

	if (!dn->config.ci_log_info)
		log_level = NLDLM_LOG_LEVEL_NONE;
	else if (dn->config.ci_log_info)
		log_level = NLDLM_LOG_LEVEL_INFO;
	else if (dn->config.ci_log_debug)
		log_level = NLDLM_LOG_LEVEL_DEBUG;

	rv = nla_put_u32(skb, NLDLM_CFG_ATTR_LOG_LEVEL, log_level);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(skb, NLDLM_CFG_ATTR_RECOVER_TIMEOUT,
			 dn->config.ci_recover_timer);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(skb, NLDLM_CFG_ATTR_INACTIVE_TIMEOUT,
			 dn->config.ci_toss_secs);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(skb, NLDLM_CFG_ATTR_DEFAULT_MARK,
			 dn->config.ci_mark);
	if (rv < 0)
		goto err;

	if (dn->config.ci_recover_callbacks) {
		rv = nla_put_flag(skb, NLDLM_CFG_ATTR_RECOVER_CALLBACKS);
		if (rv < 0)
			goto err;
	}

	mutex_unlock(&dn->cfg_lock);
	nla_nest_end(skb, nl_cfg);
	genlmsg_end(skb, hdr);
	return genlmsg_reply(skb, info);

err:
	mutex_unlock(&dn->cfg_lock);
	nla_nest_cancel(skb, nl_cfg);
	genlmsg_cancel(skb, hdr);
	nlmsg_free(skb);
	return rv;
}

static const struct nla_policy nldlm_cfg_policy[NLDLM_CFG_ATTR_MAX + 1] = {
	[NLDLM_CFG_ATTR_OUR_NODE] = { .type = NLA_NESTED },
	[NLDLM_CFG_ATTR_CLUSTER_NAME] = { .type = NLA_NUL_STRING },
	[NLDLM_CFG_ATTR_PROTOCOL] = { .type = NLA_U32 },
	[NLDLM_CFG_ATTR_PORT] = { .type = NLA_BE16 },
	[NLDLM_CFG_ATTR_RECOVER_TIMEOUT] = { .type = NLA_U32 },
	[NLDLM_CFG_ATTR_INACTIVE_TIMEOUT] = { .type = NLA_U32 },
	[NLDLM_CFG_ATTR_LOG_LEVEL] = { .type = NLA_U32 },
	[NLDLM_CFG_ATTR_DEFAULT_MARK] = { .type = NLA_U32 },
	[NLDLM_CFG_ATTR_RECOVER_CALLBACKS] = { .type = NLA_FLAG },
};

static int nldlm_set_cluster_name(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_CLUSTER_NAME])
		return -EINVAL;

	mutex_lock(&dn->cfg_lock);
	nla_strscpy(dn->config.ci_cluster_name,
		    cfg_attrs[NLDLM_CFG_ATTR_CLUSTER_NAME],
		    DLM_LOCKSPACE_LEN);
	mutex_unlock(&dn->cfg_lock);
	return 0;
}

static int nldlm_set_our_node_id(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *node_attrs[NLDLM_NODE_ATTR_MAX + 1];
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	__le32 nodeid;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_OUR_NODE])
		return -EINVAL;

	if (nla_parse_nested(node_attrs, NLDLM_NODE_ATTR_MAX,
			     cfg_attrs[NLDLM_CFG_ATTR_OUR_NODE],
			     nldlm_node_policy, NULL))
		return -EINVAL;

	nodeid = nla_get_le32(node_attrs[NLDLM_NODE_ATTR_ID]);
	return dlm_cfg_set_our_node(dn, le32_to_cpu(nodeid));
}

static int nldlm_set_protocol(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	uint32_t protocol;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_PROTOCOL])
		return -EINVAL;

	protocol = nla_get_u32(cfg_attrs[NLDLM_CFG_ATTR_PROTOCOL]);
	return dlm_cfg_set_protocol(dn, protocol);
}

static int nldlm_set_port(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	__be16 port;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_PORT])
		return -EINVAL;

	port = nla_get_be16(cfg_attrs[NLDLM_CFG_ATTR_PORT]);
	return dlm_cfg_set_port(dn, port);
}

static int nldlm_set_log_level(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	uint32_t level;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_LOG_LEVEL])
		return -EINVAL;

	level = nla_get_u32(cfg_attrs[NLDLM_CFG_ATTR_LOG_LEVEL]);
	switch (level) {
	case NLDLM_LOG_LEVEL_NONE:
		dlm_cfg_set_log_info(dn, 0);
		dlm_cfg_set_log_debug(dn, 0);
		break;
	case NLDLM_LOG_LEVEL_INFO:
		dlm_cfg_set_log_info(dn, 1);
		dlm_cfg_set_log_debug(dn, 0);
		break;
	case NLDLM_LOG_LEVEL_DEBUG:
		dlm_cfg_set_log_info(dn, 1);
		dlm_cfg_set_log_debug(dn, 1);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int nldlm_set_inactive_timeout(struct sk_buff *msg,
				      struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	uint32_t secs;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_INACTIVE_TIMEOUT])
		return -EINVAL;

	secs = nla_get_u32(cfg_attrs[NLDLM_CFG_ATTR_INACTIVE_TIMEOUT]);
	return dlm_cfg_set_toss_secs(dn, secs);
}

static int nldlm_set_recover_timeout(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	uint32_t secs;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_RECOVER_TIMEOUT])
		return -EINVAL;

	secs = nla_get_u32(cfg_attrs[NLDLM_CFG_ATTR_RECOVER_TIMEOUT]);
	return dlm_cfg_set_recover_timer(dn, secs);
}

static int nldlm_set_default_mark(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	uint32_t mark;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	if (!cfg_attrs[NLDLM_CFG_ATTR_DEFAULT_MARK])
		return -EINVAL;

	mark = nla_get_u32(cfg_attrs[NLDLM_CFG_ATTR_DEFAULT_MARK]);
	return dlm_cfg_set_mark(dn, mark);
}

static int nldlm_set_cap_recover_callbacks(struct sk_buff *msg, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(msg->sk));
	struct nlattr *cfg_attrs[NLDLM_CFG_ATTR_MAX + 1];
	int on;

	if (!info->attrs[NLDLM_ATTR_CFG])
		return -EINVAL;

	if (nla_parse_nested(cfg_attrs, NLDLM_CFG_ATTR_MAX,
			     info->attrs[NLDLM_ATTR_CFG],
			     nldlm_cfg_policy, NULL))
		return -EINVAL;

	on = nla_get_flag(cfg_attrs[NLDLM_CFG_ATTR_RECOVER_CALLBACKS]);
	return dlm_cfg_set_recover_callbacks(dn, on);
}

static const struct nla_policy nldlm_policy[NLDLM_ATTR_MAX + 1] = {
	[NLDLM_ATTR_LS] = { .type = NLA_NESTED },
	[NLDLM_ATTR_LS_MEMBER] = { .type = NLA_NESTED },
	[NLDLM_ATTR_LS_EVENT] = { .type = NLA_NESTED },
	[NLDLM_ATTR_NODE] = { .type = NLA_NESTED },
};

static const struct genl_ops nldlm_ops[] = {
	{
		.cmd = NLDLM_CMD_GET_LS,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP_STRICT,
		.doit = nldlm_get_ls,
		.dumpit = nldlm_dump_ls,
	},
	{
		.cmd = NLDLM_CMD_GET_NODE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP_STRICT,
		.doit = nldlm_get_node,
		.dumpit = nldlm_dump_nodes,
	},
	{
		.cmd = NLDLM_CMD_NEW_NODE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_new_node,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_DEL_NODE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_del_node,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_GET_LS_MEMBER,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP_STRICT,
		.doit = nldlm_get_member,
		.dumpit = nldlm_dump_members,
	},
	{
		.cmd = NLDLM_CMD_LS_EVENT_ADD_MEMBER,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_ls_member_add,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_LS_EVENT_DEL_MEMBER,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_ls_member_del,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_LS_EVENT_DONE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_ls_event_done,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_LS_EVENT_CTRL,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_ls_control,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_GET_CONFIG,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_get_config,
	},
	{
		.cmd = NLDLM_CMD_SET_CLUSTER_NAME,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_cluster_name,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_OUR_NODE,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_our_node_id,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_PROTOCOL,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_protocol,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_PORT,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_port,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_LOG_LEVEL,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_log_level,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_INACTIVE_TIMEOUT,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_inactive_timeout,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_RECOVER_TIMEOUT,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_recover_timeout,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_DEFAULT_MARK,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_default_mark,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = NLDLM_CMD_SET_RECOVER_CALLBACKS,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = nldlm_set_cap_recover_callbacks,
		.flags = GENL_ADMIN_PERM,
	},
};

static struct genl_family nldlm_genl_family __ro_after_init = {
	.name = NLDLM_GENL_NAME,
	.version = 1,
	.maxattr = NLDLM_ATTR_MAX,
	.policy = nldlm_policy,
	.netnsok = true,
	.parallel_ops = true,
	.module = THIS_MODULE,
	.ops = nldlm_ops,
	.n_ops = ARRAY_SIZE(nldlm_ops),
	.resv_start_op = NLDLM_CMD_MAX + 1,
	.mcgrps = nldlm_mcgrps,
	.n_mcgrps = ARRAY_SIZE(nldlm_mcgrps),
};

int __init dlm_nldlm_init(void)
{
	return genl_register_family(&nldlm_genl_family);
}

void dlm_nldlm_exit(void)
{
	genl_unregister_family(&nldlm_genl_family);
}
