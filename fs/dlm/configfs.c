// SPDX-License-Identifier: GPL-2.0-only
/******************************************************************************
 ******************************************************************************
 **
 **  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
 **  Copyright (C) 2004-2011 Red Hat, Inc.  All rights reserved.
 **
 **
 ******************************************************************************
 ******************************************************************************/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/configfs.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/dlmconstants.h>
#include <net/ipv6.h>
#include <net/sock.h>

#include "configfs.h"
#include "midcomms.h"
#include "lowcomms.h"
#include "config.h"

/*
 * /config/dlm/<cluster>/spaces/<space>/nodes/<node>/nodeid (refers to <node>)
 * /config/dlm/<cluster>/spaces/<space>/nodes/<node>/weight
 * /config/dlm/<cluster>/comms/<comm>/nodeid (refers to <comm>)
 * /config/dlm/<cluster>/comms/<comm>/local
 * /config/dlm/<cluster>/comms/<comm>/addr      (write only)
 * /config/dlm/<cluster>/comms/<comm>/addr_list (read only)
 * The <cluster> level is useless, but I haven't figured out how to avoid it.
 */

static struct config_group *space_list;
static struct config_group *comm_list;

struct dlm_clusters;
struct dlm_cluster;
struct dlm_spaces;
struct dlm_space;
struct dlm_comms;
struct dlm_comm;
struct dlm_nodes;
struct dlm_node;

static struct config_group *make_cluster(struct config_group *, const char *);
static void drop_cluster(struct config_group *, struct config_item *);
static void release_cluster(struct config_item *);
static struct config_group *make_space(struct config_group *, const char *);
static void drop_space(struct config_group *, struct config_item *);
static void release_space(struct config_item *);
static struct config_item *make_comm(struct config_group *, const char *);
static void drop_comm(struct config_group *, struct config_item *);
static void release_comm(struct config_item *);
static struct config_item *make_node(struct config_group *, const char *);
static void drop_node(struct config_group *, struct config_item *);
static void release_node(struct config_item *);

static struct configfs_attribute *comm_attrs[];
static struct configfs_attribute *node_attrs[];

struct dlm_cluster {
	struct config_group group;
	struct dlm_spaces *sps;
	struct dlm_comms *cms;
};

static struct dlm_cluster *config_item_to_cluster(struct config_item *i)
{
	return i ? container_of(to_config_group(i), struct dlm_cluster, group) :
		   NULL;
}

enum {
	CLUSTER_ATTR_TCP_PORT = 0,
	CLUSTER_ATTR_BUFFER_SIZE,
	CLUSTER_ATTR_RSBTBL_SIZE,
	CLUSTER_ATTR_RECOVER_TIMER,
	CLUSTER_ATTR_TOSS_SECS,
	CLUSTER_ATTR_SCAN_SECS,
	CLUSTER_ATTR_LOG_DEBUG,
	CLUSTER_ATTR_LOG_INFO,
	CLUSTER_ATTR_PROTOCOL,
	CLUSTER_ATTR_MARK,
	CLUSTER_ATTR_NEW_RSB_COUNT,
	CLUSTER_ATTR_RECOVER_CALLBACKS,
	CLUSTER_ATTR_CLUSTER_NAME,
};

static ssize_t cluster_cluster_name_show(struct config_item *item, char *buf)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	int rv;

	mutex_lock(&dn->cfg_lock);
	rv = sprintf(buf, "%s\n", dn->config.ci_cluster_name);
	mutex_unlock(&dn->cfg_lock);

	return rv;
}

static ssize_t cluster_cluster_name_store(struct config_item *item,
					  const char *buf, size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);

	mutex_lock(&dn->cfg_lock);
	strscpy(dn->config.ci_cluster_name, buf,
		sizeof(dn->config.ci_cluster_name));
	mutex_unlock(&dn->cfg_lock);

	return len;
}

CONFIGFS_ATTR(cluster_, cluster_name);

static ssize_t cluster_tcp_port_show(struct config_item *item, char *buf)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	int rv;

	mutex_lock(&dn->cfg_lock);
	rv = sprintf(buf, "%u\n", be16_to_cpu(dn->config.ci_tcp_port));
	mutex_unlock(&dn->cfg_lock);

	return rv;
}

static ssize_t cluster_tcp_port_store(struct config_item *item,
				      const char *buf, size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	int rc;
	u16 x;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	rc = kstrtou16(buf, 0, &x);
	if (rc)
		return rc;

	rc = dlm_cfg_set_port(dn, cpu_to_be16(x));
	if (rc)
		return rc;

	return len;
}

CONFIGFS_ATTR(cluster_, tcp_port);

static ssize_t cluster_set(int (*setter)(struct dlm_net *dn, unsigned int x),
			   const char *buf, size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int x;
	int rc;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	rc = kstrtouint(buf, 0, &x);
	if (rc)
		return rc;

	rc = setter(dn, x);
	if (rc)
		return rc;

	return len;
}

#define CLUSTER_ATTR(name)                                                    \
static ssize_t cluster_##name##_store(struct config_item *item, \
		const char *buf, size_t len) \
{                                                                             \
	return cluster_set(dlm_cfg_set_##name, buf, len);                     \
}                                                                             \
static ssize_t cluster_##name##_show(struct config_item *item, char *buf)     \
{                                                                             \
	struct dlm_net *dn = dlm_pernet(&init_net);                           \
	int rv;                                                               \
	mutex_lock(&dn->cfg_lock);                                            \
	rv = snprintf(buf, PAGE_SIZE, "%u\n", dn->config.ci_##name);          \
	mutex_unlock(&dn->cfg_lock);                                          \
	return rv;                                                            \
}                                                                             \
CONFIGFS_ATTR(cluster_, name)

CLUSTER_ATTR(buffer_size);
CLUSTER_ATTR(recover_timer);
CLUSTER_ATTR(toss_secs);
CLUSTER_ATTR(log_debug);
CLUSTER_ATTR(log_info);
CLUSTER_ATTR(protocol);
CLUSTER_ATTR(mark);
CLUSTER_ATTR(recover_callbacks);

#define CLUSTER_ATTR_UNUSED(name)						\
static ssize_t cluster_##name##_store(struct config_item *item,			\
		const char *buf, size_t len)					\
{										\
	struct dlm_net *dn = dlm_pernet(&init_net);				\
	unsigned int x;								\
	int rc;									\
										\
	if (!capable(CAP_SYS_ADMIN))						\
		return -EPERM;							\
	rc = kstrtouint(buf, 0, &x);						\
	if (rc)									\
		return rc;							\
										\
	dn->config.ci_##name = x;						\
	return len;								\
}										\
static ssize_t cluster_##name##_show(struct config_item *item, char *buf)	\
{										\
	struct dlm_net *dn = dlm_pernet(&init_net);				\
	return snprintf(buf, PAGE_SIZE, "%u\n", dn->config.ci_##name);		\
}										\
CONFIGFS_ATTR(cluster_, name)

CLUSTER_ATTR_UNUSED(rsbtbl_size);
CLUSTER_ATTR_UNUSED(scan_secs);
CLUSTER_ATTR_UNUSED(new_rsb_count);

static struct configfs_attribute *cluster_attrs[] = {
	[CLUSTER_ATTR_TCP_PORT] = &cluster_attr_tcp_port,
	[CLUSTER_ATTR_BUFFER_SIZE] = &cluster_attr_buffer_size,
	[CLUSTER_ATTR_RSBTBL_SIZE] = &cluster_attr_rsbtbl_size,
	[CLUSTER_ATTR_RECOVER_TIMER] = &cluster_attr_recover_timer,
	[CLUSTER_ATTR_TOSS_SECS] = &cluster_attr_toss_secs,
	[CLUSTER_ATTR_SCAN_SECS] = &cluster_attr_scan_secs,
	[CLUSTER_ATTR_LOG_DEBUG] = &cluster_attr_log_debug,
	[CLUSTER_ATTR_LOG_INFO] = &cluster_attr_log_info,
	[CLUSTER_ATTR_PROTOCOL] = &cluster_attr_protocol,
	[CLUSTER_ATTR_MARK] = &cluster_attr_mark,
	[CLUSTER_ATTR_NEW_RSB_COUNT] = &cluster_attr_new_rsb_count,
	[CLUSTER_ATTR_RECOVER_CALLBACKS] = &cluster_attr_recover_callbacks,
	[CLUSTER_ATTR_CLUSTER_NAME] = &cluster_attr_cluster_name,
	NULL,
};

enum {
	COMM_ATTR_NODEID = 0,
	COMM_ATTR_LOCAL,
	COMM_ATTR_ADDR,
	COMM_ATTR_ADDR_LIST,
	COMM_ATTR_MARK,
};

enum {
	NODE_ATTR_NODEID = 0,
	NODE_ATTR_WEIGHT,
};

struct dlm_clusters {
	struct configfs_subsystem subsys;
};

struct dlm_spaces {
	struct config_group ss_group;
};

struct dlm_space {
	struct config_group group;
	struct dlm_nodes *nds;
};

struct dlm_comms {
	struct config_group cs_group;
};

struct dlm_comm {
	struct config_item item;
};

struct dlm_nodes {
	struct config_group ns_group;
};

struct dlm_node {
	struct config_item item;
};

static struct configfs_group_operations clusters_ops = {
	.make_group = make_cluster,
	.drop_item = drop_cluster,
};

static struct configfs_item_operations cluster_ops = {
	.release = release_cluster,
};

static struct configfs_group_operations spaces_ops = {
	.make_group = make_space,
	.drop_item = drop_space,
};

static struct configfs_item_operations space_ops = {
	.release = release_space,
};

static struct configfs_group_operations comms_ops = {
	.make_item = make_comm,
	.drop_item = drop_comm,
};

static struct configfs_item_operations comm_ops = {
	.release = release_comm,
};

static struct configfs_group_operations nodes_ops = {
	.make_item = make_node,
	.drop_item = drop_node,
};

static struct configfs_item_operations node_ops = {
	.release = release_node,
};

static const struct config_item_type clusters_type = {
	.ct_group_ops = &clusters_ops,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type cluster_type = {
	.ct_item_ops = &cluster_ops,
	.ct_attrs = cluster_attrs,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type spaces_type = {
	.ct_group_ops = &spaces_ops,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type space_type = {
	.ct_item_ops = &space_ops,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type comms_type = {
	.ct_group_ops = &comms_ops,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type comm_type = {
	.ct_item_ops = &comm_ops,
	.ct_attrs = comm_attrs,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type nodes_type = {
	.ct_group_ops = &nodes_ops,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type node_type = {
	.ct_item_ops = &node_ops,
	.ct_attrs = node_attrs,
	.ct_owner = THIS_MODULE,
};

static struct dlm_space *config_item_to_space(struct config_item *i)
{
	return i ? container_of(to_config_group(i), struct dlm_space, group) :
		   NULL;
}

static struct dlm_comm *config_item_to_comm(struct config_item *i)
{
	return i ? container_of(i, struct dlm_comm, item) : NULL;
}

static struct dlm_node *config_item_to_node(struct config_item *i)
{
	return i ? container_of(i, struct dlm_node, item) : NULL;
}

static struct config_group *make_cluster(struct config_group *g,
					 const char *name)
{
	struct dlm_cluster *cl = NULL;
	struct dlm_spaces *sps = NULL;
	struct dlm_comms *cms = NULL;

	cl = kzalloc(sizeof(struct dlm_cluster), GFP_NOFS);
	sps = kzalloc(sizeof(struct dlm_spaces), GFP_NOFS);
	cms = kzalloc(sizeof(struct dlm_comms), GFP_NOFS);

	if (!cl || !sps || !cms)
		goto fail;

	cl->sps = sps;
	cl->cms = cms;

	config_group_init_type_name(&cl->group, name, &cluster_type);
	config_group_init_type_name(&sps->ss_group, "spaces", &spaces_type);
	config_group_init_type_name(&cms->cs_group, "comms", &comms_type);

	configfs_add_default_group(&sps->ss_group, &cl->group);
	configfs_add_default_group(&cms->cs_group, &cl->group);

	space_list = &sps->ss_group;
	comm_list = &cms->cs_group;
	return &cl->group;

 fail:
	kfree(cl);
	kfree(sps);
	kfree(cms);
	return ERR_PTR(-ENOMEM);
}

static void drop_cluster(struct config_group *g, struct config_item *i)
{
	struct dlm_cluster *cl = config_item_to_cluster(i);

	configfs_remove_default_groups(&cl->group);

	space_list = NULL;
	comm_list = NULL;

	config_item_put(i);
}

static void release_cluster(struct config_item *i)
{
	struct dlm_cluster *cl = config_item_to_cluster(i);

	kfree(cl->sps);
	kfree(cl->cms);
	kfree(cl);
}

static struct config_group *make_space(struct config_group *g, const char *name)
{
	struct dlm_space *sp = NULL;
	struct dlm_nodes *nds = NULL;

	sp = kzalloc(sizeof(struct dlm_space), GFP_NOFS);
	nds = kzalloc(sizeof(struct dlm_nodes), GFP_NOFS);

	if (!sp || !nds)
		goto fail;

	config_group_init_type_name(&sp->group, name, &space_type);

	config_group_init_type_name(&nds->ns_group, "nodes", &nodes_type);
	configfs_add_default_group(&nds->ns_group, &sp->group);
	return &sp->group;

 fail:
	kfree(sp);
	kfree(nds);
	return ERR_PTR(-ENOMEM);
}

static void drop_space(struct config_group *g, struct config_item *i)
{
	struct dlm_space *sp = config_item_to_space(i);

	/* assert list_empty(&sp->members) */

	configfs_remove_default_groups(&sp->group);
	config_item_put(i);
}

static void release_space(struct config_item *i)
{
	struct dlm_space *sp = config_item_to_space(i);

	kfree(sp->nds);
	kfree(sp);
}

static struct config_item *make_comm(struct config_group *g, const char *name)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid;
	struct dlm_comm *cm;
	int rv;

	rv = kstrtouint(name, 0, &nodeid);
	if (rv)
		return ERR_PTR(rv);

	cm = kzalloc(sizeof(struct dlm_comm), GFP_NOFS);
	if (!cm)
		return ERR_PTR(-ENOMEM);

	rv = dlm_cfg_new_node(dn, nodeid, 0, NULL, 0);
	if (rv) {
		kfree(cm);
		return ERR_PTR(rv);
	}

	config_item_init_type_name(&cm->item, name, &comm_type);
	return &cm->item;
}

static void drop_comm(struct config_group *g, struct config_item *i)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid;
	int rv;

	rv = kstrtouint(config_item_name(i), 0, &nodeid);
	if (WARN_ON(rv))
		return;

	rv = dlm_cfg_del_node(dn, nodeid);
	if (WARN_ON(rv))
		return;

	config_item_put(i);
}

static void release_comm(struct config_item *i)
{
	struct dlm_comm *cm = config_item_to_comm(i);

	kfree(cm);
}

static struct config_item *make_node(struct config_group *g, const char *name)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid;
	struct dlm_node *nd;
	const char *lsname;
	int rv;

	rv = kstrtouint(name, 0, &nodeid);
	if (rv)
		return ERR_PTR(rv);

	nd = kzalloc(sizeof(struct dlm_node), GFP_NOFS);
	if (!nd)
		return ERR_PTR(-ENOMEM);

	lsname = config_item_name(g->cg_item.ci_parent);
	rv = dlm_cfg_add_member(dn, lsname, nodeid, DLM_DEFAULT_WEIGHT);
	if (rv) {
		kfree(nd);
		return ERR_PTR(rv);
	}

	config_item_init_type_name(&nd->item, name, &node_type);
	return &nd->item;
}

static void drop_node(struct config_group *g, struct config_item *i)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid;
	const char *lsname;
	int rv;

	rv = kstrtouint(config_item_name(i), 0, &nodeid);
	if (WARN_ON(rv))
		return;

	lsname = config_item_name(g->cg_item.ci_parent);
	rv = dlm_cfg_del_member(dn, lsname, nodeid);
	if (WARN_ON(rv))
		return;

	config_item_put(i);
}

static void release_node(struct config_item *i)
{
	struct dlm_node *nd = config_item_to_node(i);

	kfree(nd);
}

static struct dlm_clusters clusters_root = {
	.subsys = {
		.su_group = {
			.cg_item = {
				.ci_namebuf = "dlm",
				.ci_type = &clusters_type,
			},
		},
	},
};

int __init dlm_configfs_init(void)
{
	config_group_init(&clusters_root.subsys.su_group);
	mutex_init(&clusters_root.subsys.su_mutex);
	return configfs_register_subsystem(&clusters_root.subsys);
}

void dlm_configfs_exit(void)
{
	configfs_unregister_subsystem(&clusters_root.subsys);
}

/*
 * Functions for user space to read/write attributes
 */

static ssize_t comm_nodeid_show(struct config_item *item, char *buf)
{
	unsigned int nodeid;
	int rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	return sprintf(buf, "%u\n", nodeid);
}

static ssize_t comm_nodeid_store(struct config_item *item, const char *buf,
				 size_t len)
{
	return len;
}

static ssize_t comm_local_show(struct config_item *item, char *buf)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid;
	int local = 0, rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	mutex_lock(&dn->cfg_lock);
	if (dn->our_node)
		local = (dn->our_node->id == nodeid);
	mutex_unlock(&dn->cfg_lock);

	return sprintf(buf, "%d\n", local);
}

static ssize_t comm_local_store(struct config_item *item, const char *buf,
				size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid;
	int rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	rv = dlm_cfg_set_our_node(dn, nodeid);
	if (rv)
		return rv;

	return len;
}

static ssize_t comm_addr_store(struct config_item *item, const char *buf,
		size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	struct sockaddr_storage addr;
	unsigned int nodeid;
	int rv;

	if (len != sizeof(struct sockaddr_storage))
		return -EINVAL;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	memcpy(&addr, buf, len);
	rv = dlm_cfg_add_addr(dn, nodeid, &addr);
	if (rv)
		return rv;

	return len;
}

static ssize_t comm_addr_list_show(struct config_item *item, char *buf)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	ssize_t s;
	ssize_t allowance;
	int i, rv;
	struct sockaddr_storage *addr;
	struct sockaddr_in *addr_in;
	struct sockaddr_in6 *addr_in6;
	struct dlm_cfg_node *nd;
	unsigned int nodeid;

	/* Taken from ip6_addr_string() defined in lib/vsprintf.c */
	char buf0[sizeof("AF_INET6	xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255\n")];

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	/* Derived from SIMPLE_ATTR_SIZE of fs/configfs/file.c */
	allowance = 4096;
	buf[0] = '\0';

	mutex_lock(&dn->cfg_lock);
	nd = dlm_cfg_get_node(dn, nodeid);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	for (i = 0; i < nd->addrs_count; i++) {
		addr = &nd->addrs[i];

		switch (addr->ss_family) {
		case AF_INET:
			addr_in = (struct sockaddr_in *)addr;
			s = sprintf(buf0, "AF_INET	%pI4\n", &addr_in->sin_addr.s_addr);
			break;
		case AF_INET6:
			addr_in6 = (struct sockaddr_in6 *)addr;
			s = sprintf(buf0, "AF_INET6	%pI6\n", &addr_in6->sin6_addr);
			break;
		default:
			s = sprintf(buf0, "%s\n", "<UNKNOWN>");
			break;
		}
		allowance -= s;
		if (allowance >= 0)
			strcat(buf, buf0);
		else {
			allowance += s;
			break;
		}
	}
	mutex_unlock(&dn->cfg_lock);

	return 4096 - allowance;
}

static ssize_t comm_mark_show(struct config_item *item, char *buf)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid, mark;
	struct dlm_cfg_node *nd;
	int rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	mutex_lock(&dn->cfg_lock);
	nd = dlm_cfg_get_node(dn, nodeid);
	if (!nd) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	mark = nd->mark;
	mutex_unlock(&dn->cfg_lock);

	return sprintf(buf, "%u\n", mark);
}

static ssize_t comm_mark_store(struct config_item *item, const char *buf,
			       size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid, mark;
	int rv;

	rv = kstrtouint(buf, 0, &mark);
	if (rv)
		return rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	rv = dlm_cfg_set_node_mark(dn, nodeid, mark);
	if (rv)
		return rv;

	return len;
}

CONFIGFS_ATTR(comm_, nodeid);
CONFIGFS_ATTR(comm_, local);
CONFIGFS_ATTR(comm_, mark);
CONFIGFS_ATTR_WO(comm_, addr);
CONFIGFS_ATTR_RO(comm_, addr_list);

static struct configfs_attribute *comm_attrs[] = {
	[COMM_ATTR_NODEID] = &comm_attr_nodeid,
	[COMM_ATTR_LOCAL] = &comm_attr_local,
	[COMM_ATTR_ADDR] = &comm_attr_addr,
	[COMM_ATTR_ADDR_LIST] = &comm_attr_addr_list,
	[COMM_ATTR_MARK] = &comm_attr_mark,
	NULL,
};

static ssize_t node_nodeid_show(struct config_item *item, char *buf)
{
	unsigned int nodeid;
	int rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	return sprintf(buf, "%u\n", nodeid);
}

static ssize_t node_nodeid_store(struct config_item *item, const char *buf,
				 size_t len)
{
	return len;
}

static ssize_t node_weight_show(struct config_item *item, char *buf)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	const struct dlm_cfg_member *mb;
	unsigned int nodeid, weight;
	const char *lsname;
	int rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	lsname = config_item_name(item->ci_parent->ci_parent);

	mutex_lock(&dn->cfg_lock);
	mb = dlm_cfg_get_ls_member(dn, lsname, nodeid);
	if (!mb) {
		mutex_unlock(&dn->cfg_lock);
		return -ENOENT;
	}

	weight = mb->weight;
	mutex_unlock(&dn->cfg_lock);

	return sprintf(buf, "%u\n", weight);
}

static ssize_t node_weight_store(struct config_item *item, const char *buf,
				 size_t len)
{
	struct dlm_net *dn = dlm_pernet(&init_net);
	unsigned int nodeid, weight;
	const char *lsname;
	int rv;

	rv = kstrtouint(buf, 0, &weight);
	if (rv)
		return rv;

	rv = kstrtouint(config_item_name(item), 0, &nodeid);
	if (WARN_ON(rv))
		return rv;

	lsname = config_item_name(item->ci_parent->ci_parent);
	rv = dlm_cfg_set_weight(dn, lsname, nodeid, weight);
	if (rv)
		return rv;

	return len;
}

CONFIGFS_ATTR(node_, nodeid);
CONFIGFS_ATTR(node_, weight);

static struct configfs_attribute *node_attrs[] = {
	[NODE_ATTR_NODEID] = &node_attr_nodeid,
	[NODE_ATTR_WEIGHT] = &node_attr_weight,
	NULL,
};
