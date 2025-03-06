// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <net/ipv6.h>
#include <net/ultraeth/uet_context.h>

#include "uet_netlink.h"

static const struct rhashtable_params uet_job_registry_rht_params = {
	.head_offset = offsetof(struct uet_job, rht_node),
	.key_offset = offsetof(struct uet_job, id),
	.key_len = sizeof(u32),
	.nelem_hint = 128,
	.automatic_shrinking = true,
};

int uet_jobs_init(struct uet_job_registry *jreg)
{
	int ret;

	mutex_init(&jreg->jobs_lock);

	ret = rhashtable_init(&jreg->jobs_hash, &uet_job_registry_rht_params);
	if (ret)
		mutex_destroy(&jreg->jobs_lock);

	return ret;
}

static int __job_associate(struct uet_job *job, struct uet_fep *fep)
{
	lockdep_assert_held_once(&job->jreg->jobs_lock);

	if (rcu_access_pointer(job->fep))
		return -EBUSY;

	WRITE_ONCE(fep->job_id, job->id);
	rcu_assign_pointer(job->fep, fep);

	return 0;
}

/* disassociate and close all PDCs related to the job */
static void __job_disassociate(struct uet_job *job)
{
	struct uet_fep *fep;

	fep = rcu_dereference_check(job->fep,
				    lockdep_is_held(&job->jreg->jobs_lock));
	if (!fep)
		return;

	WRITE_ONCE(fep->job_id, 0);
	RCU_INIT_POINTER(job->fep, NULL);
	synchronize_rcu();
	uet_pds_clean_job(&fep->context->pds, job->id);
}

struct uet_job *uet_job_find(struct uet_job_registry *jreg, u32 id)
{
	return rhashtable_lookup_fast(&jreg->jobs_hash, &id,
				      uet_job_registry_rht_params);
}

static struct uet_job *uet_job_find_svc_name(struct uet_job_registry *jreg,
					     char *service_name)
{
	struct uet_job *job;

	lockdep_assert_held_once(&jreg->jobs_lock);

	hlist_for_each_entry(job, &jreg->jobs_list, hnode) {
		if (!strcmp(job->service_name, service_name))
			return job;
	}

	return NULL;
}

static void __uet_job_remove(struct uet_job *job)
{
	struct uet_job_registry *jreg = job->jreg;

	__job_disassociate(job);
	hlist_del_init_rcu(&job->hnode);
	rhashtable_remove_fast(&jreg->jobs_hash, &job->rht_node,
			       uet_job_registry_rht_params);
	kfree_rcu(job, rcu);
}

bool uet_job_remove(struct uet_job_registry *jreg, u32 job_id)
{
	bool removed = false;
	struct uet_job *job;

	mutex_lock(&jreg->jobs_lock);
	job = uet_job_find(jreg, job_id);
	if (job) {
		__uet_job_remove(job);
		removed = true;
	}
	mutex_unlock(&jreg->jobs_lock);

	return removed;
}

void uet_jobs_uninit(struct uet_job_registry *jreg)
{
	struct hlist_node *tmp;
	struct uet_job *job;

	mutex_lock(&jreg->jobs_lock);
	hlist_for_each_entry_safe(job, tmp, &jreg->jobs_list, hnode)
		__uet_job_remove(job);
	mutex_unlock(&jreg->jobs_lock);

	rhashtable_destroy(&jreg->jobs_hash);
	rcu_barrier();
	mutex_destroy(&jreg->jobs_lock);
}

struct uet_job *uet_job_create(struct uet_job_registry *jreg,
			       struct uet_job_ctrl_addr_req *job_req)
{
	struct uet_job *job;
	int ret;

	if (job_req->job_id == 0)
		return ERR_PTR(-EINVAL);

	mutex_lock(&jreg->jobs_lock);
	if (uet_job_find_svc_name(jreg, job_req->service_name)) {
		mutex_unlock(&jreg->jobs_lock);
		return ERR_PTR(-EEXIST);
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return ERR_PTR(-ENOMEM);

	job->jreg = jreg;
	job->id = job_req->job_id;
	strscpy(job->service_name, job_req->service_name, sizeof(job->service_name));

	ret = rhashtable_lookup_insert_fast(&jreg->jobs_hash, &job->rht_node,
					    uet_job_registry_rht_params);
	if (ret) {
		kfree_rcu(job, rcu);
		mutex_unlock(&jreg->jobs_lock);
		return ERR_PTR(ret);
	}
	hlist_add_head_rcu(&job->hnode, &jreg->jobs_list);
	mutex_unlock(&jreg->jobs_lock);

	return job;
}

int uet_job_reg_associate(struct uet_job_registry *jreg, struct uet_fep *fep,
			  char *service_name)
{
	struct uet_job *job;
	int ret = -ENOENT;

	mutex_lock(&jreg->jobs_lock);
	job = uet_job_find_svc_name(jreg, service_name);
	if (job)
		ret = __job_associate(job, fep);
	mutex_unlock(&jreg->jobs_lock);

	return ret;
}

void uet_job_reg_disassociate(struct uet_job_registry *jreg, u32 job_id)
{
	struct uet_job *job;

	mutex_lock(&jreg->jobs_lock);
	job = uet_job_find(jreg, job_id);
	if (job)
		__job_disassociate(job);
	mutex_unlock(&jreg->jobs_lock);
}

/* returns <0 (error) or 1 (queued the skb) */
int uet_job_fep_queue_skb(struct uet_context *ctx,
			  u32 job_id, struct sk_buff *skb,
			  __be32 remote_fep_addr)
{
	struct uet_job *job = uet_job_find(&ctx->job_reg, job_id);
	struct uet_fep *fep;

	if (!job)
		return -ENOENT;

	fep = rcu_dereference(job->fep);
	if (!fep)
		return -ENODEV;

	skb_dst_drop(skb);
	skb_queue_tail(&fep->rxq, skb);

	return 1;
}

static int __nl_fep_addr_fill_one(struct sk_buff *skb,
				  const struct fep_in_address *fep_addr,
				  int fep_attr)
{
	struct nlattr *nest;
	int attr, len;

	if (!fep_addr->family)
		return 0;

	nest = nla_nest_start(skb, fep_attr);
	if (!nest)
		return -EMSGSIZE;

	switch (fep_addr->family) {
	case AF_INET:
		attr = ULTRAETH_A_FEP_IN_ADDR_IP;
		len = sizeof(fep_addr->ip);
		break;
	case AF_INET6:
		attr = ULTRAETH_A_FEP_IN_ADDR_IP6;
		len = sizeof(fep_addr->ip6);
		break;
	default:
		WARN_ON_ONCE(1);
		nla_nest_cancel(skb, nest);
		return 0;
	}

	if (nla_put(skb, attr, len, &fep_addr->ip) ||
	    nla_put_u16(skb, ULTRAETH_A_FEP_IN_ADDR_FAMILY, fep_addr->family)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);

	return 0;
}

static int __nl_uet_addr_fill_one(struct sk_buff *skb,
				    const struct fep_address *addr, int attr)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, attr);
	if (!nest)
		return -EMSGSIZE;
	if (__nl_fep_addr_fill_one(skb, &addr->in_address,
				   ULTRAETH_A_FEP_ADDRESS_IN_ADDRESS) ||
	    nla_put_u16(skb, ULTRAETH_A_FEP_ADDRESS_FLAGS, addr->flags) ||
	    nla_put_u16(skb, ULTRAETH_A_FEP_ADDRESS_CAPS, addr->fep_caps) ||
	    nla_put_u16(skb, ULTRAETH_A_FEP_ADDRESS_START_RESOURCE_INDEX,
			addr->start_resource_index) ||
	    nla_put_u16(skb, ULTRAETH_A_FEP_ADDRESS_NUM_RESOURCE_INDICES,
			addr->num_resource_indices) ||
	    nla_put_u32(skb, ULTRAETH_A_FEP_ADDRESS_INITIATOR_ID,
			addr->initiator_id) ||
	    nla_put_u16(skb, ULTRAETH_A_FEP_ADDRESS_PID_ON_FEP,
			addr->pid_on_fep) ||
	    nla_put_u8(skb, ULTRAETH_A_FEP_ADDRESS_VERSION, addr->version)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}
	nla_nest_end(skb, nest);

	return 0;
}

static int __nl_fep_fill_one(struct sk_buff *skb,
			     const struct uet_fep *fep, int attr)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, attr);
	if (!nest)
		return -EMSGSIZE;
	if (__nl_uet_addr_fill_one(skb, &fep->addr, ULTRAETH_A_FEP_ENTRY_ADDRESS)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}
	nla_nest_end(skb, nest);

	return 0;
}

static int __nl_job_feps_fill(struct sk_buff *skb, const struct uet_fep *fep)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, ULTRAETH_A_JOB_FLIST);
	if (!nest)
		return -EMSGSIZE;
	if (fep && __nl_fep_fill_one(skb, fep, ULTRAETH_A_FLIST_FEP)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}
	nla_nest_end(skb, nest);

	return 0;
}

static int __nl_job_fill_one(struct sk_buff *skb, const struct uet_job *job)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, ULTRAETH_A_JLIST_JOB);
	if (!nest)
		return -EMSGSIZE;

	if (__nl_uet_addr_fill_one(skb, &job->addr, ULTRAETH_A_JOB_ADDRESS) ||
	    nla_put_u32(skb, ULTRAETH_A_JOB_ID, job->id) ||
	    nla_put_string(skb, ULTRAETH_A_JOB_SERVICE_NAME, job->service_name) ||
	    __nl_job_feps_fill(skb, rcu_dereference(job->fep))) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);
	return 0;
}

int ultraeth_nl_job_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	int idx = 0, s_idx = cb->args[0], err;
	struct uet_context *ctx;
	struct uet_job *job;
	struct nlattr *nest;
	int context_id;
	void *hdr;

	if (!info->attrs[ULTRAETH_A_JOBS_CONTEXT_ID]) {
		NL_SET_ERR_MSG(info->extack, "context id must be specified");
		return -EINVAL;
	}

	context_id = nla_get_s32(info->attrs[ULTRAETH_A_JOBS_CONTEXT_ID]);
	ctx = uet_context_get_by_id(context_id);
	if (!ctx) {
		NL_SET_ERR_MSG(info->extack, "context doesn't exist");
		return -ENOENT;
	}

	/* filled all, return 0 */
	if (s_idx == atomic_read(&ctx->job_reg.jobs_hash.nelems))
		goto out_put;

	err = -EMSGSIZE;
	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &ultraeth_nl_family, NLM_F_MULTI, ULTRAETH_CMD_JOB_GET);
	if (!hdr)
		goto out_put;
	if (nla_put_s32(skb, ULTRAETH_A_JOBS_CONTEXT_ID, ctx->id))
		goto out_end;
	nest = nla_nest_start(skb, ULTRAETH_A_JOBS_JLIST);
	if (!nest)
		goto out_end;
	err = 0;
	rcu_read_lock();
	hlist_for_each_entry_rcu(job, &ctx->job_reg.jobs_list, hnode) {
		if (idx < s_idx) {
			idx++;
			continue;
		}
		err = __nl_job_fill_one(skb, job);
		if (err)
			break;
		idx++;
	}
	cb->args[0] = idx;
	rcu_read_unlock();
	nla_nest_end(skb, nest);
out_end:
	genlmsg_end(skb, hdr);
out_put:
	uet_context_put(ctx);

	return err ? err : skb->len;
}

int ultraeth_nl_job_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct uet_job_ctrl_addr_req jreq;
	struct uet_context *ctx;
	int context_id, job_id;
	struct uet_job *job;
	char *service_name;
	int ret = 0;

	if (!info->attrs[ULTRAETH_A_JOB_REQ_CONTEXT_ID]) {
		NL_SET_ERR_MSG(info->extack, "context id must be specified");
		return -EINVAL;
	}
	if (!info->attrs[ULTRAETH_A_JOB_REQ_ID]) {
		NL_SET_ERR_MSG(info->extack, "Job id must be specified");
		return -EINVAL;
	}
	if (!info->attrs[ULTRAETH_A_JOB_REQ_SERVICE_NAME]) {
		NL_SET_ERR_MSG(info->extack, "Job service name must be specified");
		return -EINVAL;
	}
	service_name = nla_data(info->attrs[ULTRAETH_A_JOB_REQ_SERVICE_NAME]);
	job_id = nla_get_u32(info->attrs[ULTRAETH_A_JOB_REQ_ID]);
	context_id = nla_get_s32(info->attrs[ULTRAETH_A_JOB_REQ_CONTEXT_ID]);
	ctx = uet_context_get_by_id(context_id);
	if (!ctx) {
		NL_SET_ERR_MSG(info->extack, "context doesn't exist");
		return -ENOENT;
	}

	memset(&jreq, 0, sizeof(jreq));
	jreq.job_id = job_id;
	strscpy(jreq.service_name, service_name, sizeof(jreq.service_name));
	job = uet_job_create(&ctx->job_reg, &jreq);
	if (IS_ERR(job))
		ret = PTR_ERR(job);

	uet_context_put(ctx);

	return ret;
}

int ultraeth_nl_job_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct uet_context *ctx;
	bool destroyed = false;
	int context_id, job_id;

	if (!info->attrs[ULTRAETH_A_JOB_REQ_CONTEXT_ID]) {
		NL_SET_ERR_MSG(info->extack, "context id must be specified");
		return -EINVAL;
	}
	if (!info->attrs[ULTRAETH_A_JOB_REQ_ID]) {
		NL_SET_ERR_MSG(info->extack, "Job id must be specified");
		return -EINVAL;
	}
	job_id = nla_get_u32(info->attrs[ULTRAETH_A_JOB_REQ_ID]);
	context_id = nla_get_s32(info->attrs[ULTRAETH_A_JOB_REQ_CONTEXT_ID]);
	ctx = uet_context_get_by_id(context_id);
	if (!ctx) {
		NL_SET_ERR_MSG(info->extack, "context doesn't exist");
		return -ENOENT;
	}

	destroyed = uet_job_remove(&ctx->job_reg, job_id);
	uet_context_put(ctx);

	return destroyed ? 0 : -ENOENT;
}
