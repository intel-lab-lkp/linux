// SPDX-License-Identifier: GPL-2.0-only

#include <net/netns/generic.h>

#include "dlm_internal.h"
#include "lockspace.h"
#include "midcomms.h"
#include "lowcomms.h"
#include "configfs.h"
#include "config.h"

const struct rhashtable_params dlm_rhash_rsb_params = {
	.nelem_hint = 3, /* start small */
	.key_len = DLM_RESNAME_MAXLEN,
	.key_offset = offsetof(struct dlm_rsb, res_name),
	.head_offset = offsetof(struct dlm_rsb, res_node),
	.automatic_shrinking = true,
};

/* Config file defaults */
#define DEFAULT_TCP_PORT       21064
#define DEFAULT_RSBTBL_SIZE     1024
#define DEFAULT_RECOVER_TIMER      5
#define DEFAULT_TOSS_SECS         10
#define DEFAULT_SCAN_SECS          5
#define DEFAULT_LOG_DEBUG          0
#define DEFAULT_LOG_INFO           1
#define DEFAULT_PROTOCOL           DLM_PROTO_TCP
#define DEFAULT_MARK               0
#define DEFAULT_NEW_RSB_COUNT    128
#define DEFAULT_RECOVER_CALLBACKS  0
#define DEFAULT_CLUSTER_NAME      ""

static int __net_init dlm_net_init(struct net *net)
{
	struct dlm_net *dn = dlm_pernet(net);

	write_pnet(&dn->net, net);
	dn->dlm_monitor_unused = 1;

	dn->config.ci_tcp_port = cpu_to_be16(DEFAULT_TCP_PORT);
	dn->config.ci_buffer_size = DLM_MAX_SOCKET_BUFSIZE;
	dn->config.ci_rsbtbl_size = DEFAULT_RSBTBL_SIZE;
	dn->config.ci_recover_timer = DEFAULT_RECOVER_TIMER;
	dn->config.ci_toss_secs = DEFAULT_TOSS_SECS;
	dn->config.ci_scan_secs = DEFAULT_SCAN_SECS;
	dn->config.ci_log_debug = DEFAULT_LOG_DEBUG;
	dn->config.ci_log_info = DEFAULT_LOG_INFO;
	dn->config.ci_protocol = DEFAULT_PROTOCOL;
	dn->config.ci_mark = DEFAULT_MARK;
	dn->config.ci_new_rsb_count = DEFAULT_NEW_RSB_COUNT;
	dn->config.ci_recover_callbacks = DEFAULT_RECOVER_CALLBACKS;
	strscpy(dn->config.ci_cluster_name, DEFAULT_CLUSTER_NAME);

	dlm_lockspace_net_init(dn);
	dlm_midcomms_init(dn);

	mutex_init(&dn->cfg_lock);
	INIT_LIST_HEAD(&dn->nodes);
	INIT_LIST_HEAD(&dn->lockspaces);

	return 0;
}

static void __net_exit dlm_net_exit(struct net *net)
{
	struct dlm_net *dn = dlm_pernet(net);

	dlm_midcomms_exit(dn);
}

static unsigned int dlm_net_id __read_mostly;

static struct pernet_operations dlm_net_ops = {
	.init = dlm_net_init,
	.exit = dlm_net_exit,
	.id = &dlm_net_id,
	.size = sizeof(struct dlm_net),
};

struct dlm_net *dlm_pernet(struct net *net)
{
	return dlm_net_id ? net_generic(net, dlm_net_id) : NULL;
}

int __init dlm_config_init(void)
{
	int rv;

	rv = register_pernet_subsys(&dlm_net_ops);
	if (rv)
		return rv;

	rv = dlm_nldlm_init();
	if (rv)
		goto err;

	rv = dlm_configfs_init();
	if (rv)
		goto err_nldlm;

	return rv;

err_nldlm:
	dlm_nldlm_exit();
err:
	unregister_pernet_subsys(&dlm_net_ops);
	dlm_net_id = 0;

	return rv;
}

void dlm_config_exit(void)
{
	dlm_configfs_exit();
	dlm_nldlm_exit();

	unregister_pernet_subsys(&dlm_net_ops);
	dlm_net_id = 0;
}

unsigned int dlm_our_nodeid(struct dlm_net *dn)
{
	return dn->our_node->id;
}

int dlm_cfg_set_cluster_name(struct dlm_net *dn, const char *name)
{
	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	strscpy(dn->config.ci_cluster_name, name);
	mutex_unlock(&dn->cfg_lock);
	return 0;
}

int dlm_cfg_set_port(struct dlm_net *dn, __be16 port)
{
	if (!port)
		return -EINVAL;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_tcp_port = port;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_buffer_size(struct dlm_net *dn, unsigned int size)
{
	if (size < DLM_MAX_SOCKET_BUFSIZE)
		return -EINVAL;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_buffer_size = size;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_protocol(struct dlm_net *dn, unsigned int protocol)
{
	switch (protocol) {
	case 0:
		/* TCP */
		break;
	case 1:
		/* SCTP */
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_protocol = protocol;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_toss_secs(struct dlm_net *dn, unsigned int secs)
{
	if (!secs)
		return -EINVAL;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_toss_secs = secs;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_recover_timer(struct dlm_net *dn, unsigned int secs)
{
	if (!secs)
		return -EINVAL;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_recover_timer = secs;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_log_debug(struct dlm_net *dn, unsigned int on)
{
	mutex_lock(&dn->cfg_lock);
	dn->config.ci_log_debug = on;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_log_info(struct dlm_net *dn, unsigned int on)
{
	mutex_lock(&dn->cfg_lock);
	dn->config.ci_log_info = on;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_mark(struct dlm_net *dn, unsigned int mark)
{
	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_mark = 1;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_recover_callbacks(struct dlm_net *dn, unsigned int on)
{
	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	dn->config.ci_recover_callbacks = on;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

struct dlm_cfg_ls *dlm_cfg_get_ls(struct dlm_net *dn, const char *lsname)
{
	struct dlm_cfg_ls *iter, *ls = NULL;

	list_for_each_entry(iter, &dn->lockspaces, list) {
		if (!strncmp(iter->name, lsname, DLM_LOCKSPACE_LEN)) {
			ls = iter;
			break;
		}
	}

	return ls;
}

/* caller must free mem */
int dlm_config_nodes(struct dlm_net *dn, char *lsname,
		     struct dlm_config_node **nodes_out,
		     unsigned int *count_out)
{
	struct dlm_config_node *nodes, *node;
	struct dlm_cfg_member *mb;
	struct dlm_cfg_ls *ls;
	int rv = 0, count;

	mutex_lock(&dn->cfg_lock);
	ls = dlm_cfg_get_ls(dn, lsname);
	if (!ls) {
		rv = -EEXIST;
		goto out;
	}

	if (!ls->members_count) {
		rv = -EINVAL;
		goto out;
	}

	count = ls->members_count;
	nodes = kcalloc(count, sizeof(struct dlm_config_node), GFP_NOFS);
	if (!nodes) {
		rv = -ENOMEM;
		goto out;
	}

	node = nodes;
	list_for_each_entry(mb, &ls->members, list) {
		node->nodeid = mb->nd->id;
		node->weight = mb->weight;
		node->new = mb->new;
		node->comm_seq = mb->nd->seq;
		node++;

		mb->new = 0;
	}

	*count_out = count;
	*nodes_out = nodes;
 out:
	mutex_unlock(&dn->cfg_lock);

	return rv;
}

struct dlm_cfg_node *dlm_cfg_get_node(struct dlm_net *dn, unsigned int id)
{
	struct dlm_cfg_node *iter, *con = NULL;

	list_for_each_entry(iter, &dn->nodes, list) {
		if (iter->id == id) {
			con = iter;
			break;
		}
	}

	return con;
}

static int dlm_cfg_set_addr(struct dlm_net *dn, struct dlm_cfg_node *nd,
			    unsigned int id, struct sockaddr_storage *addr)
{
	int rv;

	/* TODO -EEXIST */
	if (nd->addrs_count >= DLM_MAX_ADDR_COUNT)
		return -ENOSPC;

	rv = dlm_midcomms_addr(dn, nd->id, addr);
	if (rv)
		return rv;

	nd->addrs[nd->addrs_count++] = *addr;

	return 0;
}

int dlm_cfg_new_node(struct dlm_net *dn, unsigned int id,
		    unsigned int mark, struct sockaddr_storage *addrs,
		    size_t addrs_count)
{
	struct dlm_cfg_node *nd;
	int i, rv = 0;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		rv = -EBUSY;
		goto out;
	}

	nd = dlm_cfg_get_node(dn, id);
	if (nd) {
		rv = -EEXIST;
		goto out;
	}

	nd = kzalloc(sizeof(*nd), GFP_ATOMIC);
	if (!nd) {
		rv = -ENOMEM;
		goto out;
	}

	nd->seq = dn->dlm_cfg_node_count++;
	if (!nd->seq)
		nd->seq = dn->dlm_cfg_node_count++;

	nd->id = id;
	nd->mark = mark;

	/* due configfs optional */
	if (addrs && addrs_count) {
		if (addrs_count >= DLM_MAX_ADDR_COUNT) {
			rv = -ENOSPC;
			kfree(nd);
			goto out;
		}

		for (i = 0; i < addrs_count; i++) {
			rv = dlm_cfg_set_addr(dn, nd, i, &addrs[i]);
			if (rv < 0) {
				kfree(nd);
				goto out;
			}
		}

		nd->addrs_count = addrs_count;
	}

	nd->idx = ++dn->node_idx;
	list_add_tail(&nd->list, &dn->nodes);

out:
	mutex_unlock(&dn->cfg_lock);

	return rv;
}

int dlm_cfg_del_node(struct dlm_net *dn, unsigned int id)
{
	struct dlm_cfg_node *nd;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	nd = dlm_cfg_get_node(dn, id);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	if (dn->our_node == nd) {
		if (nd->used != 1) {
			mutex_unlock(&dn->cfg_lock);
			return -EBUSY;
		}

		dn->our_node = NULL;
	} else {
		if (nd->used != 0) {
			mutex_unlock(&dn->cfg_lock);
			return -EBUSY;
		}
	}

	list_del(&nd->list);
	dlm_midcomms_close(dn, id);
	mutex_unlock(&dn->cfg_lock);

	kfree(nd);

	return 0;
}

int dlm_cfg_set_our_node(struct dlm_net *dn, unsigned int id)
{
	struct dlm_cfg_node *nd;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	nd = dlm_cfg_get_node(dn, id);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	if (dn->our_node)
		dn->our_node->used--;

	dn->our_node = nd;
	nd->used++;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

static struct dlm_cfg_member *
dlm_cfg_get_member(struct dlm_cfg_ls *ls, unsigned int id)
{
	struct dlm_cfg_member *iter, *mb = NULL;

	list_for_each_entry(iter, &ls->members, list) {
		if (iter->nd->id == id) {
			mb = iter;
			break;
		}
	}

	return mb;
}

struct dlm_cfg_member *
dlm_cfg_get_ls_member(struct dlm_net *dn, const char *lsname,
		      unsigned int nodeid)
{
	struct dlm_cfg_ls *ls;

	ls = dlm_cfg_get_ls(dn, lsname);
	if (!ls)
		return NULL;

	return dlm_cfg_get_member(ls, nodeid);
}

int dlm_cfg_add_member(struct dlm_net *dn, const char *lsname,
		       unsigned int id, unsigned int weight)
{
	struct dlm_cfg_member *mb;
	struct dlm_cfg_node *nd;
	struct dlm_cfg_ls *ls;
	bool new_ls = false;

	mutex_lock(&dn->cfg_lock);
	ls = dlm_cfg_get_ls(dn, lsname);
	if (!ls) {
		ls = kzalloc(sizeof(*ls), GFP_ATOMIC);
		if (!ls) {
			mutex_unlock(&dn->cfg_lock);
			return -ENOMEM;
		}

		strscpy(ls->name, lsname);
		INIT_LIST_HEAD(&ls->members);
		ls->idx = ++dn->ls_idx;
		new_ls = true;
	} else {
		mb = dlm_cfg_get_member(ls, id);
		if (mb) {
			mutex_unlock(&dn->cfg_lock);
			return -EEXIST;
		}
	}

	nd = dlm_cfg_get_node(dn, id);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		if (new_ls)
			kfree(ls);
		return -ENOENT;
	}

	mb = kzalloc(sizeof(*mb), GFP_ATOMIC);
	if (!mb) {
		mutex_unlock(&dn->cfg_lock);
		if (new_ls)
			kfree(ls);
		return -ENOMEM;
	}

	nd->used++;
	mb->nd = nd;
	mb->ls = ls;
	mb->weight = weight;
	mb->new = 1;

	list_add_tail(&mb->list, &ls->members);
	ls->members_count++;
	mb->idx = ++ls->member_idx;

	if (new_ls)
		list_add_tail(&ls->list, &dn->lockspaces);
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_del_member(struct dlm_net *dn, const char *lsname, unsigned int id)
{
	struct dlm_cfg_member *mb;

	mutex_lock(&dn->cfg_lock);
	mb = dlm_cfg_get_ls_member(dn, lsname, id);
	if (!mb) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	mb->nd->used--;
	list_del(&mb->list);
	mb->ls->members_count--;
	if (!mb->ls->members_count)
		list_del(&mb->ls->list);
	mutex_unlock(&dn->cfg_lock);

	kfree(mb);

	return 0;
}

int dlm_cfg_add_addr(struct dlm_net *dn, unsigned int id,
		     struct sockaddr_storage *addr)
{
	struct dlm_cfg_node *nd;
	int rv;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	nd = dlm_cfg_get_node(dn, id);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	rv = dlm_cfg_set_addr(dn, nd, id, addr);
	mutex_unlock(&dn->cfg_lock);

	return rv;
}

int dlm_comm_seq(struct dlm_net *dn, unsigned int id, uint32_t *seq)
{
	struct dlm_cfg_node *nd;

	mutex_lock(&dn->cfg_lock);
	nd = dlm_cfg_get_node(dn, id);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	*seq = nd->seq;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

/* num 0 is first addr, num 1 is second addr */
int dlm_our_addr(struct dlm_net *dn, struct sockaddr_storage *addr, int num)
{
	mutex_lock(&dn->cfg_lock);
	if (!dn->our_node) {
		mutex_unlock(&dn->cfg_lock);
		return -1;
	}

	if (num >= dn->our_node->addrs_count) {
		mutex_unlock(&dn->cfg_lock);
		return -1;
	}

	memcpy(addr, &dn->our_node->addrs[num], sizeof(*addr));
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_node_mark(struct dlm_net *dn, unsigned int nodeid,
			  unsigned int mark)
{
	struct dlm_cfg_node *nd;

	mutex_lock(&dn->cfg_lock);
	if (dlm_lowcomms_is_running(dn)) {
		mutex_unlock(&dn->cfg_lock);
		return -EBUSY;
	}

	nd = dlm_cfg_get_node(dn, nodeid);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	nd->mark = mark;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}

int dlm_cfg_set_weight(struct dlm_net *dn, const char *lsname,
		       unsigned int nodeid, unsigned int weight)
{
	struct dlm_cfg_member *mb;

	mutex_lock(&dn->cfg_lock);
	mb = dlm_cfg_get_ls_member(dn, lsname, nodeid);
	if (!mb) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	mb->weight = weight;
	mutex_unlock(&dn->cfg_lock);

	return 0;
}
