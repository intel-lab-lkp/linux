/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_P4TC_H
#define __NET_P4TC_H

#include <uapi/linux/p4tc.h>
#include <linux/workqueue.h>
#include <net/sch_generic.h>
#include <net/net_namespace.h>
#include <linux/refcount.h>
#include <linux/rhashtable.h>
#include <linux/rhashtable-types.h>
#include <net/tc_act/p4tc.h>
#include <net/p4tc_types.h>

#define P4TC_DEFAULT_NUM_TABLES P4TC_MINTABLES_COUNT
#define P4TC_DEFAULT_MAX_RULES 1
#define P4TC_PATH_MAX 3
#define P4TC_MAX_TENTRIES 33554432
#define P4TC_DEFAULT_TENTRIES 256
#define P4TC_MAX_TMASKS 1024
#define P4TC_DEFAULT_TMASKS 8
#define P4TC_MAX_T_AGING 864000000
#define P4TC_DEFAULT_T_AGING 30000

#define P4TC_MAX_PERMISSION (GENMASK(P4TC_PERM_MAX_BIT, 0))

#define P4TC_KERNEL_PIPEID 0

#define P4TC_PID_IDX 0
#define P4TC_TBLID_IDX 1
#define P4TC_AID_IDX 1
#define P4TC_PARSEID_IDX 1

struct p4tc_dump_ctx {
	u32 ids[P4TC_PATH_MAX];
	struct rhashtable_iter *iter;
};

struct p4tc_template_common;

struct p4tc_path_nlattrs {
	char                     *pname;
	u32                      *ids;
	bool                     pname_passed;
};

struct p4tc_pipeline;
struct p4tc_template_ops {
	void (*init)(void);
	struct p4tc_template_common *(*cu)(struct net *net, struct nlmsghdr *n,
					   struct nlattr *nla,
					   struct p4tc_path_nlattrs *nl_pname,
					   struct netlink_ext_ack *extack);
	int (*put)(struct p4tc_pipeline *pipeline,
		   struct p4tc_template_common *tmpl,
		   struct netlink_ext_ack *extack);
	int (*gd)(struct net *net, struct sk_buff *skb, struct nlmsghdr *n,
		  struct nlattr *nla, struct p4tc_path_nlattrs *nl_pname,
		  struct netlink_ext_ack *extack);
	int (*fill_nlmsg)(struct net *net, struct sk_buff *skb,
			  struct p4tc_template_common *tmpl,
			  struct netlink_ext_ack *extack);
	int (*dump)(struct sk_buff *skb, struct p4tc_dump_ctx *ctx,
		    struct nlattr *nla, char **p_name, u32 *ids,
		    struct netlink_ext_ack *extack);
	int (*dump_1)(struct sk_buff *skb, struct p4tc_template_common *common);
};

struct p4tc_template_common {
	char                     name[TEMPLATENAMSZ];
	struct p4tc_template_ops *ops;
	u32                      p_id;
	u32                      PAD0;
};

extern const struct p4tc_template_ops p4tc_pipeline_ops;

struct p4tc_pipeline {
	struct p4tc_template_common common;
	struct idr                  p_act_idr;
	struct idr                  p_tbl_idr;
	struct rcu_head             rcu;
	struct net                  *net;
	u32                         num_created_acts;
	/* Accounts for how many entities are referencing this pipeline.
	 * As for now only P4 filters can refer to pipelines.
	 */
	refcount_t                  p_ctrl_ref;
	u16                         num_tables;
	u16                         curr_tables;
	u8                          p_state;
};

struct p4tc_pipeline_net {
	struct idr pipeline_idr;
};

static inline bool p4tc_tmpl_msg_is_update(struct nlmsghdr *n)
{
	return n->nlmsg_type == RTM_UPDATEP4TEMPLATE;
}

int p4tc_tmpl_generic_dump(struct sk_buff *skb, struct p4tc_dump_ctx *ctx,
			   struct idr *idr, int idx,
			   struct netlink_ext_ack *extack);

struct p4tc_pipeline *p4tc_pipeline_find_byany(struct net *net,
					       const char *p_name,
					       const u32 pipeid,
					       struct netlink_ext_ack *extack);
struct p4tc_pipeline *p4tc_pipeline_find_byid(struct net *net,
					      const u32 pipeid);
struct p4tc_pipeline *p4tc_pipeline_find_get(struct net *net,
					     const char *p_name,
					     const u32 pipeid,
					     struct netlink_ext_ack *extack);

static inline bool p4tc_pipeline_get(struct p4tc_pipeline *pipeline)
{
	return refcount_inc_not_zero(&pipeline->p_ctrl_ref);
}

void p4tc_pipeline_put(struct p4tc_pipeline *pipeline);
struct p4tc_pipeline *
p4tc_pipeline_find_byany_unsealed(struct net *net, const char *p_name,
				  const u32 pipeid,
				  struct netlink_ext_ack *extack);

struct p4tc_act *tcf_p4_find_act(struct net *net,
				 const struct tc_action_ops *a_o,
				 struct netlink_ext_ack *extack);
void
tcf_p4_put_prealloc_act(struct p4tc_act *act, struct tcf_p4act *p4_act);

static inline bool pipeline_sealed(struct p4tc_pipeline *pipeline)
{
	return pipeline->p_state == P4TC_STATE_READY;
}

static inline int p4tc_action_destroy(struct tc_action **acts)
{
	struct tc_action *acts_non_prealloc[TCA_ACT_MAX_PRIO] = {NULL};
	int ret = 0;

	if (acts) {
		int j = 0;
		int i;

		for (i = 0; i < TCA_ACT_MAX_PRIO && acts[i]; i++) {
			if (acts[i]->tcfa_flags & TCA_ACT_FLAGS_PREALLOC) {
				const struct tc_action_ops *ops;
				struct tcf_p4act *p4act;
				struct p4tc_act *act;
				struct net *net;

				p4act = (struct tcf_p4act *)acts[i];
				net = acts[i]->idrinfo->net;
				ops = acts[i]->ops;

				act = tcf_p4_find_act(net, ops, NULL);
				tcf_p4_put_prealloc_act(act, p4act);
			} else {
				acts_non_prealloc[j] = acts[i];
				j++;
			}
		}

		ret = tcf_action_destroy(acts_non_prealloc, TCA_ACT_UNBIND);
		kfree(acts);
	}

	return ret;
}

#define P4TC_CONTROL_PERMISSIONS (GENMASK(13, 7))
#define P4TC_DATA_PERMISSIONS (GENMASK(6, 0))

#define P4TC_TABLE_PERMISSIONS                                   \
	((GENMASK(P4TC_CTRL_PERM_C_BIT, P4TC_CTRL_PERM_D_BIT)) | \
	 P4TC_CTRL_PERM_P | P4TC_CTRL_PERM_S | P4TC_DATA_PERM_R | \
	 P4TC_DATA_PERM_X)

#define P4TC_PERMISSIONS_UNINIT (1 << P4TC_PERM_MAX_BIT)

struct p4tc_table_defact {
	struct tc_action **default_acts;
	/* Will have two 7 bits blocks containing CRUDXPS (Create, read, update,
	 * delete, execute, publish and subscribe) permissions for control plane
	 * and data plane. The first 5 bits are for control and the next five
	 * are for data plane. |crudxpscrudxps| if we were to denote it as UNIX
	 * permission flags.
	 */
	__u16 permissions;
	struct rcu_head  rcu;
};

struct p4tc_table_perm {
	__u16           permissions;
	struct rcu_head rcu;
};

struct p4tc_table {
	struct p4tc_template_common         common;
	struct list_head                    tbl_acts_list;
	struct idr                          tbl_masks_idr;
	struct idr                          tbl_prio_idr;
	struct rhltable                     tbl_entries;
	struct p4tc_table_defact __rcu      *tbl_default_hitact;
	struct p4tc_table_defact __rcu      *tbl_default_missact;
	struct p4tc_table_perm __rcu        *tbl_permissions;
	struct p4tc_table_entry_mask __rcu  **tbl_masks_array;
	unsigned long __rcu                 *tbl_free_masks_bitmap;
	u64                                 tbl_aging;
	/* Locks the available masks IDR which will be used when adding and
	 * deleting table entries.
	 */
	spinlock_t                          tbl_masks_idr_lock;
	u32                                 tbl_keysz;
	u32                                 tbl_id;
	u32                                 tbl_max_entries;
	u32                                 tbl_max_masks;
	u32                                 tbl_curr_num_masks;
	/* Accounts for how many entities refer to this table. Usually just the
	 * pipeline it belongs to.
	 */
	refcount_t                          tbl_ctrl_ref;
	u16                                 tbl_type;
};

extern const struct p4tc_template_ops p4tc_table_ops;

struct p4tc_act_param {
	struct list_head head;
	struct rcu_head	rcu;
	void            *value;
	void            *mask;
	struct p4tc_type *type;
	u32             id;
	u32             index;
	u16             bitend;
	u8              flags;
	u8              PAD0;
	char            name[ACTPARAMNAMSIZ];
};

struct p4tc_act_param_ops {
	int (*init_value)(struct net *net, struct p4tc_act_param_ops *op,
			  struct p4tc_act_param *nparam, struct nlattr **tb,
			  struct netlink_ext_ack *extack);
	int (*dump_value)(struct sk_buff *skb, struct p4tc_act_param_ops *op,
			  struct p4tc_act_param *param);
	void (*free)(struct p4tc_act_param *param);
	u32 len;
	u32 alloc_len;
};

struct p4tc_act {
	struct p4tc_template_common common;
	struct tc_action_ops        ops;
	struct tc_action_net        *tn;
	struct p4tc_pipeline        *pipeline;
	struct idr                  params_idr;
	struct tcf_exts             exts;
	struct list_head            head;
	struct list_head            prealloc_list;
	/* Locks the preallocated actions list.
	 * The list will be used whenever a table entry with an action or a
	 * table default action gets created, updated or deleted. Note that
	 * table entries may be added by both control and data path, so the
	 * list can be modified from both contexts.
	 */
	spinlock_t                  list_lock;
	u32                         a_id;
	u32                         num_params;
	u32                         num_prealloc_acts;
	/* Accounts for how many entities refer to this action. Usually just the
	 * pipeline it belongs to.
	 */
	refcount_t                  a_ref;
	bool                        active;
	char                        full_act_name[ACTNAMSIZ];
};

struct p4tc_table_act {
	struct list_head node;
	struct tc_action_ops *ops;
	u8     flags;
};

extern const struct p4tc_template_ops p4tc_act_ops;

static inline int p4tc_action_init(struct net *net, struct nlattr *nla,
				   struct tc_action *acts[], u32 pipeid,
				   u32 flags, struct netlink_ext_ack *extack)
{
	int init_res[TCA_ACT_MAX_PRIO];
	size_t attrs_size;
	int ret;
	int i;

	/* If action was already created, just bind to existing one*/
	flags |= TCA_ACT_FLAGS_BIND;
	flags |= TCA_ACT_FLAGS_FROM_P4TC;
	ret = tcf_action_init(net, NULL, nla, NULL, acts, init_res, &attrs_size,
			      flags, 0, extack);

	/* Check if we are trying to bind to dynamic action from different pipeline */
	for (i = 0; i < TCA_ACT_MAX_PRIO && acts[i]; i++) {
		struct tc_action *a = acts[i];
		struct tcf_p4act *p;

		if (a->ops->id <= TCA_ID_MAX)
			continue;

		p = to_p4act(a);
		if (p->p_id != pipeid) {
			NL_SET_ERR_MSG(extack,
				       "Unable to bind to dynact from different pipeline");
			ret = -EPERM;
			goto destroy_acts;
		}
	}

	return ret;

destroy_acts:
	p4tc_action_destroy(acts);
	return ret;
}

struct p4tc_act *p4tc_action_find_get(struct p4tc_pipeline *pipeline,
				      const char *act_name, const u32 a_id,
				      struct netlink_ext_ack *extack);
struct p4tc_act *p4tc_action_find_byid(struct p4tc_pipeline *pipeline,
				       const u32 a_id);

static inline bool p4tc_action_put_ref(struct p4tc_act *act)
{
	return refcount_dec_not_one(&act->a_ref);
}

struct p4tc_act_param *tcf_param_find_byid(struct idr *params_idr,
					   const u32 param_id);
struct p4tc_act_param *tcf_param_find_byany(struct p4tc_act *act,
					    const char *param_name,
					    const u32 param_id,
					    struct netlink_ext_ack *extack);

struct p4tc_table *p4tc_table_find_byany(struct p4tc_pipeline *pipeline,
					 const char *tblname, const u32 tbl_id,
					 struct netlink_ext_ack *extack);
struct p4tc_table *p4tc_table_find_byid(struct p4tc_pipeline *pipeline,
					const u32 tbl_id);
int p4tc_table_try_set_state_ready(struct p4tc_pipeline *pipeline,
				   struct netlink_ext_ack *extack);
void p4tc_table_put_mask_array(struct p4tc_pipeline *pipeline);
struct p4tc_table *p4tc_table_find_get(struct p4tc_pipeline *pipeline,
				       const char *tblname, const u32 tbl_id,
				       struct netlink_ext_ack *extack);

static inline bool p4tc_table_put_ref(struct p4tc_table *table)
{
	return refcount_dec_not_one(&table->tbl_ctrl_ref);
}

struct p4tc_table_default_act_params {
	struct p4tc_table_defact *default_hitact;
	struct p4tc_table_defact *default_missact;
	struct nlattr *default_hit_attr;
	struct nlattr *default_miss_attr;
};

int
p4tc_table_init_default_acts(struct net *net,
			     struct p4tc_table_default_act_params *def_params,
			     struct p4tc_table *table,
			     struct list_head *acts_list,
			     struct netlink_ext_ack *extack);

static inline void
p4tc_table_defacts_acts_copy(struct p4tc_table_defact *defact_copy,
			     struct p4tc_table_defact *defact_orig)
{
	defact_copy->default_acts = defact_orig->default_acts;
}

void
p4tc_table_replace_default_acts(struct p4tc_table *table,
				struct p4tc_table_default_act_params *def_params,
				bool lock_rtnl);

struct p4tc_table_perm *
p4tc_table_init_permissions(struct p4tc_table *table, u16 permissions,
			    struct netlink_ext_ack *extack);
void p4tc_table_replace_permissions(struct p4tc_table *table,
				    struct p4tc_table_perm *tbl_perm,
				    bool lock_rtnl);

struct tcf_p4act *
tcf_p4_get_next_prealloc_act(struct p4tc_act *act);
void tcf_p4_set_init_flags(struct tcf_p4act *p4act);

#define to_pipeline(t) ((struct p4tc_pipeline *)t)
#define to_act(t) ((struct p4tc_act *)t)
#define to_table(t) ((struct p4tc_table *)t)

#endif
