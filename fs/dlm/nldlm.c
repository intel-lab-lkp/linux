// SPDX-License-Identifier: GPL-2.0-only

#include <net/genetlink.h>
#include <net/sock.h>

#include "dlm_internal.h"
#include "nldlm-kernel.h"
#include "lockspace.h"
#include "member.h"
#include "config.h"
#include "lock.h"

static int nldlm_put_ls_event(struct sk_buff *msg, const struct dlm_ls *ls,
			      u32 portid, u32 seq, int flags, uint32_t cmd)
{
	void *hdr;
	int rv;

	hdr = genlmsg_put(msg, 0, 0, &nldlm_nl_family, 0, cmd);
	if (!hdr)
		return -ENOBUFS;

	rv = nla_put_string(msg, NLDLM_A_LS_NAME, ls->ls_name);
	if (rv < 0)
		goto err;

	genlmsg_end(msg, hdr);
	return 0;

err:
	genlmsg_cancel(msg, hdr);
	return -ENOBUFS;
}

int nldlm_ls_event(const struct dlm_ls *ls, uint32_t cmd)
{
	struct sk_buff *msg;
	int rv;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!msg)
		return -ENOMEM;

	rv = nldlm_put_ls_event(msg, ls, 0, 0, 0, cmd);
	if (rv < 0) {
		nlmsg_free(msg);
		return rv;
	}

	return genlmsg_multicast_netns(&nldlm_nl_family,
				       read_pnet(&ls->ls_dn->net), msg, 0,
				       NLDLM_NLGRP_LS_EVENT, GFP_ATOMIC);
}

static int __nldlm_get_ls(struct sk_buff *msg, struct dlm_cfg_ls *ls,
			  u32 portid, u32 seq, struct netlink_callback *cb,
			  int flags)
{
	void *hdr;
	int rv;

	hdr = genlmsg_put(msg, portid, seq, &nldlm_nl_family, flags,
			  NLDLM_CMD_GET_LS);
	if (!hdr)
		return -EMSGSIZE;

	if (cb)
		genl_dump_check_consistent(cb, hdr);

	rv = nla_put_string(msg, NLDLM_A_LS_NAME, ls->name);
	if (rv < 0)
		goto err;

	genlmsg_end(msg, hdr);
	return 0;

err:
	genlmsg_cancel(msg, hdr);
	return rv;
}

int nldlm_nl_get_ls_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	struct dlm_cfg_ls *ls = NULL, *ls_iter;
	char lsname[DLM_LOCKSPACE_LEN];
	struct sk_buff *msg;
	int rv;

	if (!info->attrs[NLDLM_A_LS_NAME])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_NAME],
		    DLM_LOCKSPACE_LEN);

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

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!msg) {
		rv = -ENOMEM;
		goto err;
	}

	rv = __nldlm_get_ls(msg, ls, info->snd_portid,
			    info->snd_seq, NULL, 0);
	if (rv < 0) {
		nlmsg_free(msg);
		goto err;
	}

	rv = genlmsg_reply(msg, info);

err:
	mutex_unlock(&dn->cfg_lock);
	return rv;
}

int nldlm_nl_get_ls_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
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

static int __nldlm_get_node(struct sk_buff *msg, struct dlm_cfg_node *nd,
			    u32 portid, u32 seq,
			    struct netlink_callback *cb, int flags)
{
	struct nlattr *nl_addr;
	void *hdr;
	size_t i;
	int rv;

	hdr = genlmsg_put(msg, portid, seq, &nldlm_nl_family, flags,
			  NLDLM_CMD_GET_NODE);
	if (!hdr)
		return -EMSGSIZE;

	if (cb)
		genl_dump_check_consistent(cb, hdr);

	rv = nla_put_le32(msg, NLDLM_A_NODE_ID, cpu_to_le32(nd->id));
	if (rv < 0)
		goto err;

	rv = nla_put_u32(msg, NLDLM_A_NODE_MARK, nd->mark);
	if (rv < 0)
		goto err;

	for (i = 0; i < nd->addrs_count; i++) {
		nl_addr = nla_nest_start(msg, NLDLM_A_NODE_ADDRS);
		if (!nl_addr)
			goto err;

		rv = nla_put_u16(msg, NLDLM_A_ADDR_FAMILY, nd->addrs[i].ss_family);
		if (rv) {
			nla_nest_cancel(msg, nl_addr);
			goto err;
		}

		switch (nd->addrs[i].ss_family) {
		case AF_INET:
			rv = nla_put_in_addr(msg, NLDLM_A_ADDR_ADDR4,
					     ((struct sockaddr_in *)&nd->addrs[i])->sin_addr.s_addr);
			if (rv) {
				nla_nest_cancel(msg, nl_addr);
				goto err;
			}

			break;
		case AF_INET6:
			rv = nla_put_in6_addr(msg, NLDLM_A_ADDR_ADDR6,
					      &((struct sockaddr_in6 *)&nd->addrs[i])->sin6_addr);
			if (rv) {
				nla_nest_cancel(msg, nl_addr);
				goto err;
			}

			break;
		default:
			nla_nest_cancel(msg, nl_addr);
			goto err;
		}

		nla_nest_end(msg, nl_addr);
	}

	genlmsg_end(msg, hdr);
	return 0;

err:
	genlmsg_cancel(msg, hdr);
	return rv;
}

int nldlm_nl_get_node_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	struct dlm_cfg_node *nd;
	struct sk_buff *msg;
	__le32 nodeid;
	int rv;

	if (!info->attrs[NLDLM_A_NODE_ID])
		return -EINVAL;

	nodeid = nla_get_le32(info->attrs[NLDLM_A_NODE_ID]);

	mutex_lock(&dn->cfg_lock);
	nd = dlm_cfg_get_node(dn, le32_to_cpu(nodeid));
	if (!nd) {
		rv = -ENOENT;
		goto out;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!msg) {
		rv = -ENOMEM;
		goto out;
	}

	rv = __nldlm_get_node(msg, nd, info->snd_portid,
			      info->snd_seq, NULL, 0);
	if (rv < 0) {
		nlmsg_free(msg);
		goto out;
	}

	rv = genlmsg_reply(msg, info);

out:
	mutex_unlock(&dn->cfg_lock);
	return rv;
}

int nldlm_nl_get_node_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
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

int nldlm_nl_add_node_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *head = nlmsg_attrdata(info->nlhdr, GENL_HDRLEN);
	struct sockaddr_storage addrs[DLM_MAX_ADDR_COUNT] = {};
	int rem, len = nlmsg_attrlen(info->nlhdr, GENL_HDRLEN);
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	struct nlattr *addr_attrs[NLDLM_A_ADDR_MAX + 1];
	struct nlattr *nla;
	size_t addrs_count;
	__le32 nodeid;
	u32 mark;
	int rv;

	if (!info->attrs[NLDLM_A_NODE_ID] ||
	    !info->attrs[NLDLM_A_NODE_ADDRS])
		return -EINVAL;

	nodeid = nla_get_le32(info->attrs[NLDLM_A_NODE_ID]);

	addrs_count = 0;
	nla_for_each_attr(nla, head, len, rem) {
		if (nla_type(nla) != NLDLM_A_NODE_ADDRS)
			continue;

		if (addrs_count == DLM_MAX_ADDR_COUNT)
			return -ENOSPC;

		rv = nla_parse_nested(addr_attrs, NLDLM_A_ADDR_MAX, nla,
				     nldlm_addr_nl_policy, NULL);
		if (rv)
			return rv;

		if (!addr_attrs[NLDLM_A_ADDR_FAMILY])
			return -EINVAL;

		addrs[addrs_count].ss_family = nla_get_u16(addr_attrs[NLDLM_A_ADDR_FAMILY]);
		switch (addrs[addrs_count].ss_family) {
		case AF_INET:
			if (!addr_attrs[NLDLM_A_ADDR_ADDR4])
				return -EINVAL;

			((struct sockaddr_in *)&addrs[addrs_count])->sin_addr.s_addr =
				nla_get_in_addr(addr_attrs[NLDLM_A_ADDR_ADDR4]);
			break;
		case AF_INET6:
			if (!addr_attrs[NLDLM_A_ADDR_ADDR6])
				return -EINVAL;

			((struct sockaddr_in6 *)&addrs[addrs_count])->sin6_addr =
				nla_get_in6_addr(addr_attrs[NLDLM_A_ADDR_ADDR6]);
			break;
		default:
			return -EINVAL;
		}

		addrs_count++;
	}

	if (info->attrs[NLDLM_A_NODE_MARK])
		mark = nla_get_u32(info->attrs[NLDLM_A_NODE_MARK]);
	else
		mark = DLM_DEFAULT_MARK;

	return dlm_cfg_new_node(dn, le32_to_cpu(nodeid), mark, addrs,
				addrs_count);
}

int nldlm_nl_del_node_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	__le32 nodeid;

	if (!info->attrs[NLDLM_A_NODE_ID])
		return -EINVAL;

	nodeid = nla_get_le32(info->attrs[NLDLM_A_NODE_ID]);
	return dlm_cfg_del_node(dn, le32_to_cpu(nodeid));
}

static int __nldlm_get_ls_member(struct sk_buff *msg,
				 const struct dlm_cfg_member *mb,
				 u32 portid, u32 seq,
				 struct netlink_callback *cb, int flags)
{
	void *hdr;
	int rv;

	hdr = genlmsg_put(msg, portid, seq, &nldlm_nl_family, flags,
			  NLDLM_CMD_GET_LS_MEMBER);
	if (!hdr)
		return -EMSGSIZE;

	if (cb)
		genl_dump_check_consistent(cb, hdr);

	rv = nla_put_string(msg, NLDLM_A_LS_MEMBER_LS_NAME, mb->ls->name);
	if (rv < 0)
		goto err;

	rv = nla_put_le32(msg, NLDLM_A_LS_MEMBER_NODEID, cpu_to_le32(mb->nd->id));
	if (rv < 0)
		goto err;

	rv = nla_put_u32(msg, NLDLM_A_LS_MEMBER_WEIGHT, mb->weight);
	if (rv < 0)
		goto err;

	genlmsg_end(msg, hdr);
	return 0;

err:
	genlmsg_cancel(msg, hdr);
	return rv;
}

int nldlm_nl_get_ls_member_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_cfg_member *mb;
	struct sk_buff *msg;
	__le32 nodeid;
	int rv;

	if (!info->attrs[NLDLM_A_LS_MEMBER_LS_NAME] ||
	    !info->attrs[NLDLM_A_LS_MEMBER_NODEID])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_MEMBER_LS_NAME],
		    DLM_LOCKSPACE_LEN);
	nodeid = nla_get_le32(info->attrs[NLDLM_A_LS_MEMBER_NODEID]);

	mutex_lock(&dn->cfg_lock);
	mb = dlm_cfg_get_ls_member(dn, lsname, le32_to_cpu(nodeid));
	if (!mb) {
		rv = -ENOENT;
		goto out;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!msg) {
		rv = -ENOMEM;
		goto out;
	}

	rv = __nldlm_get_ls_member(msg, mb, info->snd_portid,
				   info->snd_seq, NULL, 0);
	if (rv < 0) {
		nlmsg_free(msg);
		goto out;
	}

	rv = genlmsg_reply(msg, info);

out:
	mutex_unlock(&dn->cfg_lock);
	return rv;
}

int nldlm_nl_get_ls_member_dumpit(struct sk_buff *skb,
				  struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	unsigned int idx = cb->args[0];
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_cfg_member *mb;
	struct dlm_cfg_ls *ls;
	int rv;

	if (!info->attrs[NLDLM_A_LS_MEMBER_LS_NAME])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_MEMBER_LS_NAME],
		    DLM_LOCKSPACE_LEN);

	mutex_lock(&dn->cfg_lock);
	ls = dlm_cfg_get_ls(dn, lsname);
	if (!ls) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	list_for_each_entry(mb, &ls->members, list) {
		if (mb->idx < idx)
			continue;

		rv = __nldlm_get_ls_member(skb, mb, NETLINK_CB(cb->skb).portid,
					   cb->nlh->nlmsg_seq, cb,
					   NLM_F_MULTI);
		if (rv < 0)
			break;

		idx = mb->idx + 1;
	}
	mutex_unlock(&dn->cfg_lock);

	cb->args[0] = idx;
	return skb->len;
}

int nldlm_nl_ls_ctrl_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	enum nldlm_ls_ctrl_action ctrl;
	struct dlm_ls *ls;

	if (!info->attrs[NLDLM_A_LS_CTRL_LS_NAME] ||
	    !info->attrs[NLDLM_A_LS_CTRL_ACTION])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_CTRL_LS_NAME],
		    DLM_LOCKSPACE_LEN);
	ctrl = nla_get_u32(info->attrs[NLDLM_A_LS_CTRL_ACTION]);

	ls = dlm_find_lockspace_name(dn, lsname);
	if (!ls)
		return -ENOENT;

	switch (ctrl) {
	case NLDLM_LS_CTRL_ACTION_STOP:
		dlm_ls_stop(ls);
		break;
	case NLDLM_LS_CTRL_ACTION_START:
		dlm_ls_start(ls);
		break;
	default:
		dlm_put_lockspace(ls);
		return -EINVAL;
	}

	dlm_put_lockspace(ls);
	return 0;
}

int nldlm_nl_ls_event_done_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	enum nldlm_ls_event_result result;
	char lsname[DLM_LOCKSPACE_LEN];
	struct dlm_ls *ls;
	__le32 global_id;
	int rv;

	if (!info->attrs[NLDLM_A_LS_EVENT_RESULT_LS_NAME] ||
	    !info->attrs[NLDLM_A_LS_EVENT_RESULT_LS_GLOBAL_ID] ||
	    !info->attrs[NLDLM_A_LS_EVENT_RESULT_RESULT])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_EVENT_RESULT_LS_NAME],
		    DLM_LOCKSPACE_LEN);
	global_id = nla_get_le32(info->attrs[NLDLM_A_LS_EVENT_RESULT_LS_GLOBAL_ID]);
	result = nla_get_u32(info->attrs[NLDLM_A_LS_EVENT_RESULT_RESULT]);

	/* sanity checking only if the global_id is already given in another
	 * lockspace. This check is racy and it requires more changes to
	 * not make it racy, however is it better than just apply the id.
	 */
	ls = dlm_find_lockspace_global(dn, le32_to_cpu(global_id));
	if (ls) {
		dlm_put_lockspace(ls);
		return -EEXIST;
	}

	ls = dlm_find_lockspace_name(dn, lsname);
	if (!ls)
		return -ENOENT;

	switch (result) {
	case NLDLM_LS_EVENT_RESULT_SUCCESS:
		ls->ls_global_id = le32_to_cpu(global_id);
		rv = 0;
		break;
	case NLDLM_LS_EVENT_RESULT_FAILURE:
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

int nldlm_nl_ls_add_member_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	__le32 nodeid;
	u32 weight = DLM_DEFAULT_WEIGHT;

	if (!info->attrs[NLDLM_A_LS_MEMBER_LS_NAME] ||
	    !info->attrs[NLDLM_A_LS_MEMBER_NODEID])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_MEMBER_LS_NAME],
		    DLM_LOCKSPACE_LEN);
	nodeid = nla_get_le32(info->attrs[NLDLM_A_LS_MEMBER_NODEID]);
	if (info->attrs[NLDLM_A_LS_MEMBER_WEIGHT])
		weight = nla_get_u32(info->attrs[NLDLM_A_LS_MEMBER_WEIGHT]);

	return dlm_cfg_add_member(dn, lsname, le32_to_cpu(nodeid), weight);
}

int nldlm_nl_ls_del_member_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	char lsname[DLM_LOCKSPACE_LEN];
	__le32 nodeid;

	if (!info->attrs[NLDLM_A_LS_MEMBER_LS_NAME] ||
	    !info->attrs[NLDLM_A_LS_MEMBER_NODEID])
		return -EINVAL;

	nla_strscpy(lsname, info->attrs[NLDLM_A_LS_MEMBER_LS_NAME],
		    DLM_LOCKSPACE_LEN);
	nodeid = nla_get_le32(info->attrs[NLDLM_A_LS_MEMBER_NODEID]);

	return dlm_cfg_del_member(dn, lsname, le32_to_cpu(nodeid));
}

int nldlm_nl_get_cfg_doit(struct sk_buff *skb, struct genl_info *info)
{
	enum nldlm_log_level log_level = NLDLM_LOG_LEVEL_NONE;
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	struct sk_buff *msg;
	void *hdr;
	int rv;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &nldlm_nl_family, 0, NLDLM_CMD_GET_CFG);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	mutex_lock(&dn->cfg_lock);
	if (dn->our_node) {
		rv = nla_put_le32(msg, NLDLM_A_CFG_OUR_NODEID,
				  cpu_to_le32(dn->our_node->id));
		if (rv < 0)
			goto err;
	}

	rv = nla_put_string(msg, NLDLM_A_CFG_CLUSTER_NAME,
			    dn->config.ci_cluster_name);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(msg, NLDLM_A_CFG_PROTOCOL,
			 dn->config.ci_protocol);
	if (rv < 0)
		goto err;

	rv = nla_put_be16(msg, NLDLM_A_CFG_PORT,
			  dn->config.ci_tcp_port);
	if (rv < 0)
		goto err;

	if (!dn->config.ci_log_info)
		log_level = NLDLM_LOG_LEVEL_NONE;
	else if (dn->config.ci_log_info)
		log_level = NLDLM_LOG_LEVEL_INFO;
	else if (dn->config.ci_log_debug)
		log_level = NLDLM_LOG_LEVEL_DEBUG;

	rv = nla_put_u32(msg, NLDLM_A_CFG_LOG_LEVEL, log_level);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(msg, NLDLM_A_CFG_RECOVER_TIMEOUT,
			 dn->config.ci_recover_timer);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(msg, NLDLM_A_CFG_INACTIVE_TIMEOUT,
			 dn->config.ci_toss_secs);
	if (rv < 0)
		goto err;

	rv = nla_put_u32(msg, NLDLM_A_CFG_DEFAULT_MARK,
			 dn->config.ci_mark);
	if (rv < 0)
		goto err;

	if (dn->config.ci_recover_callbacks) {
		rv = nla_put_flag(msg, NLDLM_A_CFG_RECOVER_CALLBACKS);
		if (rv < 0)
			goto err;
	}

	mutex_unlock(&dn->cfg_lock);
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

err:
	mutex_unlock(&dn->cfg_lock);
	genlmsg_cancel(msg, hdr);
	nlmsg_free(msg);
	return rv;
}

int nldlm_nl_set_our_nodeid_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	__le32 nodeid;

	if (!info->attrs[NLDLM_A_CFG_OUR_NODEID])
		return -EINVAL;

	nodeid = nla_get_le32(info->attrs[NLDLM_A_CFG_OUR_NODEID]);

	return dlm_cfg_set_our_nodeid(dn, le32_to_cpu(nodeid));
}

int nldlm_nl_set_cluster_name_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));

	if (!info->attrs[NLDLM_A_CFG_CLUSTER_NAME])
		return -EINVAL;

	mutex_lock(&dn->cfg_lock);
	nla_strscpy(dn->config.ci_cluster_name,
		    info->attrs[NLDLM_A_CFG_CLUSTER_NAME],
		    DLM_LOCKSPACE_LEN);
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int nldlm_nl_set_protocol_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	uint32_t protocol;

	if (!info->attrs[NLDLM_A_CFG_PROTOCOL])
		return -EINVAL;

	protocol = nla_get_u32(info->attrs[NLDLM_A_CFG_PROTOCOL]);

	return dlm_cfg_set_protocol(dn, protocol);
}

int nldlm_nl_set_port_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	__be16 port;

	if (!info->attrs[NLDLM_A_CFG_PORT])
		return -EINVAL;

	port = nla_get_be16(info->attrs[NLDLM_A_CFG_PORT]);

	return dlm_cfg_set_port(dn, port);
}

int nldlm_nl_set_recover_timeout_doit(struct sk_buff *skb,
				      struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	uint32_t secs;

	if (!info->attrs[NLDLM_A_CFG_RECOVER_TIMEOUT])
		return -EINVAL;

	secs = nla_get_u32(info->attrs[NLDLM_A_CFG_RECOVER_TIMEOUT]);

	return dlm_cfg_set_recover_timer(dn, secs);
}

int nldlm_nl_set_inactive_timeout_doit(struct sk_buff *skb,
				       struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	uint32_t secs;

	if (!info->attrs[NLDLM_A_CFG_INACTIVE_TIMEOUT])
		return -EINVAL;

	secs = nla_get_u32(info->attrs[NLDLM_A_CFG_INACTIVE_TIMEOUT]);

	return dlm_cfg_set_toss_secs(dn, secs);
}

int nldlm_nl_set_log_level_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	uint32_t level;

	if (!info->attrs[NLDLM_A_CFG_LOG_LEVEL])
		return -EINVAL;

	level = nla_get_u32(info->attrs[NLDLM_A_CFG_LOG_LEVEL]);

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

int nldlm_nl_set_default_mark_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	uint32_t mark;

	if (!info->attrs[NLDLM_A_CFG_DEFAULT_MARK])
		return -EINVAL;

	mark = nla_get_u32(info->attrs[NLDLM_A_CFG_DEFAULT_MARK]);

	return dlm_cfg_set_mark(dn, mark);
}

int nldlm_nl_set_recover_callbacks_doit(struct sk_buff *skb,
					struct genl_info *info)
{
	struct dlm_net *dn = dlm_pernet(sock_net(skb->sk));
	int flag;

	flag = nla_get_flag(info->attrs[NLDLM_A_CFG_RECOVER_CALLBACKS]);

	return dlm_cfg_set_recover_callbacks(dn, flag);
}

int __init dlm_nldlm_init(void)
{
	return genl_register_family(&nldlm_nl_family);
}

void dlm_nldlm_exit(void)
{
	genl_unregister_family(&nldlm_nl_family);
}
