/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UET_JOB_H
#define _UET_JOB_H

#include <linux/types.h>
#include <linux/rhashtable.h>
#include <linux/skbuff.h>
#include <linux/mutex.h>
#include <uapi/linux/ultraeth.h>

struct uet_context;

struct uet_job_registry {
	struct mutex jobs_lock;
	struct hlist_head jobs_list;
	struct rhashtable jobs_hash;
};

struct uet_fep {
	struct uet_context *context;
	struct sk_buff_head rxq;
	struct fep_address addr;
	u32 ack_gen_trigger;
	u32 ack_gen_min_pkt_add;
	u32 job_id;
};

/**
 * struct uet_job - single job
 *
 * @rht_node: link into the job registry's job hash table
 * @hnode: link into the job registry's list
 * @jreg: pointer to job registry (owner)
 * @service_name: service name used for lookups on address req
 * @addr: job specific address (XXX)
 * @job_id: unique job id
 * @rcu: used for freeing
 *
 * if @fep is set then the job is considered associated, i.e. there is
 * an fd for the context's character device which is bound to this
 * job (FEP)
 */
struct uet_job {
	struct rhash_head rht_node;
	struct hlist_node hnode;

	struct uet_job_registry *jreg;

	char service_name[UET_SVC_MAX_LEN];

	struct fep_address addr;
	struct uet_fep __rcu *fep;

	u32 id;

	struct rcu_head rcu;
};

struct uet_job_ctrl_addr_req {
	char service_name[UET_SVC_MAX_LEN];
	struct fep_in_address address;
	__u32 job_id;
	__u32 os_pid;
	__u8 flags;
};

int uet_jobs_init(struct uet_job_registry *jreg);
void uet_jobs_uninit(struct uet_job_registry *jreg);

struct uet_job *uet_job_create(struct uet_job_registry *jreg,
			       struct uet_job_ctrl_addr_req *job_req);
bool uet_job_remove(struct uet_job_registry *jreg, u32 job_id);
struct uet_job *uet_job_find(struct uet_job_registry *jreg, u32 id);
void uet_job_reg_disassociate(struct uet_job_registry *jreg, u32 job_id);
int uet_job_reg_associate(struct uet_job_registry *jreg, struct uet_fep *fep,
			  char *service_name);
int uet_job_fep_queue_skb(struct uet_context *ctx, u32 job_id,
			  struct sk_buff *skb, __be32 remote_fep_addr);
#endif /* _UET_JOB_H */
