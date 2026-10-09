// SPDX-License-Identifier: GPL-2.0
/* Ordered forwarding queue for HSR and PRP.
 *
 * One bounded FIFO and one consumer per instance preserve the submission
 * order of locally numbered frames to every lower device without a
 * forwarding lock.  Master TX, interlink RX and internally generated
 * supervision frames are submitted before any sequence number is
 * assigned; an idle caller executes its own input inline, contended
 * inputs are drained in submission order by one BH work item.
 */

#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <net/dst.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "hsr_main.h"
#include "hsr_forward.h"

struct hsr_job {
	struct list_head	list;
	struct sk_buff		*skb;
	struct net_device	*dev;	/* held entry device */
	enum hsr_port_type	type;	/* entry role at submit */
	enum hsr_job_class	class;	/* ordinary / internal supervision */
	int			depth;	/* dev_recursion_level() at submit */
	unsigned int		charge;	/* immutable skb->truesize */
};

/* Queue-stage drops share the mapping in hsr_forward.c. */

static void hsr_job_drop(struct hsr_job *job)
{
	/* Drop statistics complete before dev_put(). */
	hsr_fwd_drop_stat(job->dev, job->type, job->class);
	kfree_skb(job->skb);
	dev_put(job->dev);
	kfree(job);
}

/* Admission under fwd_lock.  Ordinary inputs must fit both the total and
 * the ordinary-subset bounds; internal supervision may use the reserve
 * within the total.  Difference comparisons avoid overflow.
 */
static bool hsr_queue_fits(struct hsr_priv *hsr, struct hsr_job *job)
{
	if (hsr->fwd_jobs >= HSR_FWD_JOBS_MAX)
		return false;
	if (job->charge > HSR_FWD_BYTES_MAX - hsr->fwd_bytes)
		return false;
	if (job->class == HSR_JOB_NORMAL) {
		if (hsr->fwd_ord_jobs >= HSR_FWD_ORD_JOBS_MAX)
			return false;
		if (job->charge > HSR_FWD_ORD_BYTES_MAX - hsr->fwd_ord_bytes)
			return false;
	}
	return true;
}

static void hsr_queue_charge_add(struct hsr_priv *hsr, struct hsr_job *job)
{
	hsr->fwd_jobs++;
	hsr->fwd_bytes += job->charge;
	if (job->class == HSR_JOB_NORMAL) {
		hsr->fwd_ord_jobs++;
		hsr->fwd_ord_bytes += job->charge;
	}
}

/* Exactly once, on dequeue to active and on purge. */
static void hsr_queue_charge_del(struct hsr_priv *hsr, struct hsr_job *job)
{
	hsr->fwd_jobs--;
	hsr->fwd_bytes -= job->charge;
	if (job->class == HSR_JOB_NORMAL) {
		hsr->fwd_ord_jobs--;
		hsr->fwd_ord_bytes -= job->charge;
	}
}

/* Re-validate role and device under RCU; no bare port/node is kept
 * across queueing.
 */
static struct hsr_port *hsr_job_port(struct hsr_priv *hsr, struct hsr_job *job)
{
	struct hsr_port *port;

	hsr_for_each_port(hsr, port)
		if (port->type == job->type && port->dev == job->dev)
			return port;
	return NULL;
}

/* Complete one input: re-validate the entry under RCU, raise the xmit
 * recursion depth to the level saved at submit (never lower it), assign
 * the supervision sequence number if needed, then run the complete
 * per-frame path.  Returns the budget units this input consumed.
 */
static unsigned int hsr_job_process(struct hsr_priv *hsr, struct hsr_job *job,
				    enum hsr_exec_source source)
{
	struct hsr_port *port;
	unsigned int raised = 0;
	unsigned int cost;

	rcu_read_lock();
	port = hsr_job_port(hsr, job);
	if (!port) {
		rcu_read_unlock();
		hsr_job_drop(job);
		return 1;
	}

	while (dev_recursion_level() < job->depth) {
		dev_xmit_recursion_inc();
		raised++;
	}

	if (job->class == HSR_JOB_INTERNAL_SUP)
		hsr_assign_sup_seq(job->skb, hsr, source);
	cost = hsr_forward_input(job->skb, port, job->class, source);

	while (raised) {
		dev_xmit_recursion_dec();
		raised--;
	}
	rcu_read_unlock();

	dev_put(job->dev);
	kfree(job);

	/* Actual per-frame units consumed; a failed aggregate costs 1. */
	return cost;
}

static void hsr_queue_owned_release(struct hsr_priv *hsr)
{
	if (!hsr->fwd_stopped && !list_empty(&hsr->fwd_queue))
		queue_work(system_bh_wq, &hsr->fwd_work);
	else
		hsr->fwd_owned = false;
}

static void hsr_queue_work(struct work_struct *work)
{
	struct hsr_priv *hsr = container_of(work, struct hsr_priv, fwd_work);
	unsigned int used = 0;

	while (used < HSR_FWD_BUDGET_MAX) {
		struct hsr_job *job;
		unsigned int cost;

		spin_lock_bh(&hsr->fwd_lock);
		if (hsr->fwd_stopped || list_empty(&hsr->fwd_queue)) {
			hsr->fwd_owned = false;
			spin_unlock_bh(&hsr->fwd_lock);
			return;
		}
		job = list_first_entry(&hsr->fwd_queue, struct hsr_job, list);
		list_del(&job->list);
		hsr_queue_charge_del(hsr, job);
		spin_unlock_bh(&hsr->fwd_lock);

		cost = hsr_job_process(hsr, job, HSR_EXEC_WORKER);
		if (cost >= HSR_FWD_BUDGET_MAX - used)
			used = HSR_FWD_BUDGET_MAX;
		else
			used += cost;
	}

	/* Budget exhausted with a backlog: keep the ownership and hand
	 * the queue to the same worker object again.
	 */
	spin_lock_bh(&hsr->fwd_lock);
	hsr_queue_owned_release(hsr);
	spin_unlock_bh(&hsr->fwd_lock);
}

static void hsr_queue_inline_one(struct hsr_priv *hsr, struct hsr_job *job)
{
	local_bh_disable();
	hsr_job_process(hsr, job, HSR_EXEC_INLINE);
	spin_lock_bh(&hsr->fwd_lock);
	hsr_queue_owned_release(hsr);
	spin_unlock_bh(&hsr->fwd_lock);
	local_bh_enable();
}

void hsr_queue_submit(struct sk_buff *skb, struct hsr_port *port,
		      enum hsr_job_class class)
{
	struct hsr_priv *hsr = port->hsr;
	struct hsr_job *job;
	bool inline_owner = false;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "HSR queue submit outside RCU read-side");

	job = kzalloc_obj(*job, GFP_ATOMIC);
	if (!job) {
		hsr_fwd_drop_stat(port->dev, port->type, class);
		kfree_skb(skb);
		return;
	}
	job->skb = skb;
	job->dev = port->dev;
	job->type = port->type;
	job->class = class;
	job->depth = dev_recursion_level();
	job->charge = skb->truesize;
	skb_dst_force(skb);
	dev_hold(job->dev);

	spin_lock_bh(&hsr->fwd_lock);
	if (hsr->fwd_stopped) {
		spin_unlock_bh(&hsr->fwd_lock);
		hsr_job_drop(job);
		return;
	}
	if (!hsr->fwd_owned) {
		/* Idle: take the execution ownership and process this
		 * input directly.  The job never enters the public queue
		 * and does not consume queued charge.
		 */
		hsr->fwd_owned = true;
		inline_owner = true;
	} else if (hsr_queue_fits(hsr, job)) {
		list_add_tail(&job->list, &hsr->fwd_queue);
		hsr_queue_charge_add(hsr, job);
	} else {
		spin_unlock_bh(&hsr->fwd_lock);
		hsr_job_drop(job);
		return;
	}
	spin_unlock_bh(&hsr->fwd_lock);

	if (inline_owner)
		hsr_queue_inline_one(hsr, job);
}

void hsr_forward_forget_port(struct hsr_priv *hsr, struct net_device *dev)
{
	struct hsr_job *job, *next;
	LIST_HEAD(purge);

	spin_lock_bh(&hsr->fwd_lock);
	list_for_each_entry_safe(job, next, &hsr->fwd_queue, list) {
		if (job->dev != dev)
			continue;
		list_move_tail(&job->list, &purge);
		hsr_queue_charge_del(hsr, job);
	}
	spin_unlock_bh(&hsr->fwd_lock);

	list_for_each_entry_safe(job, next, &purge, list) {
		list_del(&job->list);
		hsr_job_drop(job);
	}
}

void hsr_forward_stop(struct hsr_priv *hsr)
{
	struct hsr_job *job, *next;
	LIST_HEAD(purge);

	spin_lock_bh(&hsr->fwd_lock);
	hsr->fwd_stopped = true;
	spin_unlock_bh(&hsr->fwd_lock);

	synchronize_net();
	cancel_work_sync(&hsr->fwd_work);

	spin_lock_bh(&hsr->fwd_lock);
	list_splice_init(&hsr->fwd_queue, &purge);
	hsr->fwd_jobs = 0;
	hsr->fwd_ord_jobs = 0;
	hsr->fwd_bytes = 0;
	hsr->fwd_ord_bytes = 0;
	hsr->fwd_owned = false;
	spin_unlock_bh(&hsr->fwd_lock);

	list_for_each_entry_safe(job, next, &purge, list) {
		list_del(&job->list);
		hsr_job_drop(job);
	}
}

void hsr_forward_init(struct hsr_priv *hsr)
{
	spin_lock_init(&hsr->fwd_lock);
	INIT_LIST_HEAD(&hsr->fwd_queue);
	INIT_WORK(&hsr->fwd_work, hsr_queue_work);
}
