// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_transport_fc.h>
#include <scsi/fc/fc_els.h>
#include <linux/list.h>
#include <linux/delay.h>
#include "ibmvfc.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

/**
 * ibmvfc_run_async_work - queue and drain an FPIN work item via a private workqueue
 * @wq:		private workqueue to use
 * @vhost:	ibmvfc host struct
 * @ae:		async CRQ event to process
 *
 * Allocates an ibmvfc_async_work, takes a scsi_host reference (released by
 * ibmvfc_process_async_work()), populates the work item, queues it on @wq
 * (which the caller owns), and flushes @wq so the work completes before
 * returning. Using a private workqueue gives ibmvfc_process_async_work() its
 * own thread stack, avoiding kernel stack overflow when called from a deep
 * KUnit context.
 */
static void ibmvfc_run_async_work(struct workqueue_struct *wq,
				  struct ibmvfc_host *vhost,
				  struct ibmvfc_async_crq *ae)
{
	struct ibmvfc_async_work *aw;

	aw = kzalloc_obj(*aw);
	if (WARN_ON(!aw))
		return;
	if (WARN_ON(!scsi_host_get(vhost->host))) {
		kfree(aw);
		return;
	}
	INIT_WORK(&aw->async_work_s, ibmvfc_process_async_work);
	aw->vhost = vhost;
	aw->crq = *ae;
	queue_work(wq, &aw->async_work_s);
	flush_workqueue(wq);
}

/**
 * ibmvfc_async_fpin_test - unit test for IBMVFC_AE_FPIN handling via
 * ibmvfc_process_async_work
 * @test: pointer to kunit structure
 *
 * Tests
 * - statistics updates on valid FPIN status values (per-target path)
 * - statistics unchanged on invalid FPIN status value
 * - host-scope delivery for zero-identifier LINK_CONGESTED/CONGESTION_CLEARED
 * - silent drop for zero-identifier per-port events (PORT_CONGESTED)
 *
 * Return: void
 */
static void ibmvfc_async_fpin_test(struct kunit *test)
{
	u64 post[IBMVFC_AE_FPIN_CONGESTION_CLEARED + 1];
	u64 pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED + 1];
	struct ibmvfc_async_crq ae = {};
	enum ibmvfc_ae_fpin_status fs;
	struct workqueue_struct *wq;
	struct fc_host_attrs *fc_host;
	struct ibmvfc_target *tgt;
	struct ibmvfc_host *vhost;
	struct fc_rport *rport;
	unsigned long flags;

	vhost = ibmvfc_get_first_vhost();
	if (!vhost)
		kunit_skip(test, "No ibmvfc devices available");

	wq = alloc_ordered_workqueue("ibmvfc-kunit-fpin", 0);
	if (!wq) {
		scsi_host_put(vhost->host);
		kunit_skip(test, "Failed to allocate workqueue");
	}

	spin_lock_irqsave(&vhost->host->host_lock, flags);
	if (vhost->scsi_scrqs.num_targets < 1) {
		spin_unlock_irqrestore(&vhost->host->host_lock, flags);
		destroy_workqueue(wq);
		scsi_host_put(vhost->host);
		kunit_skip(test, "No targets");
	}
	tgt = NULL;
	rport = NULL;
	list_for_each_entry(tgt, &vhost->scsi_scrqs.targets, queue) {
		if (tgt->rport &&
		    tgt->rport->port_state == FC_PORTSTATE_ONLINE &&
		    (tgt->rport->roles & (FC_PORT_ROLE_FCP_TARGET | FC_PORT_ROLE_NVME_TARGET))) {
			rport = tgt->rport;
			get_device(&rport->dev);
			kref_get(&tgt->kref);
			break;
		}
	}
	if (!rport) {
		spin_unlock_irqrestore(&vhost->host->host_lock, flags);
		destroy_workqueue(wq);
		scsi_host_put(vhost->host);
		kunit_skip(test, "No suitable online target rport found");
	}
	spin_unlock_irqrestore(&vhost->host->host_lock, flags);

	fc_host = shost_to_fc_host(vhost->host);

	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(rport->fpin_stats.cn_clear);
	pre[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(rport->fpin_stats.li_failure_unknown);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	for (fs = IBMVFC_AE_FPIN_LINK_CONGESTED; fs <= IBMVFC_AE_FPIN_CONGESTION_CLEARED; fs++) {
		ae.valid = 0x80;
		ae.link_state = IBMVFC_AE_LS_LINK_UP;
		ae.fpin_status = fs;
		ae.event = cpu_to_be64(IBMVFC_AE_FPIN);
		ae.scsi_id = cpu_to_be64(tgt->scsi_id);
		ae.wwpn = cpu_to_be64(tgt->wwpn);
		ae.node_name = cpu_to_be64(tgt->ids.node_name);
		ibmvfc_run_async_work(wq, vhost, &ae);
	}

	post[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(rport->fpin_stats.cn_clear);
	post[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(rport->fpin_stats.li_failure_unknown);
	post[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_LINK_CONGESTED],
			pre[IBMVFC_AE_FPIN_LINK_CONGESTED]+1);
	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_PORT_CONGESTED],
			pre[IBMVFC_AE_FPIN_PORT_CONGESTED]+1);
	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_PORT_CLEARED],
			pre[IBMVFC_AE_FPIN_PORT_CLEARED]+1);
	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_PORT_DEGRADED],
			pre[IBMVFC_AE_FPIN_PORT_DEGRADED]+1);
	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_CONGESTION_CLEARED],
			pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED]+1);

	/* bad path */
	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(rport->fpin_stats.cn_clear);
	pre[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(rport->fpin_stats.li_failure_unknown);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	ae.valid = 0x80;
	ae.link_state = IBMVFC_AE_LS_LINK_UP;
	ae.fpin_status = 0; /* bad value */
	ae.event = cpu_to_be64(IBMVFC_AE_FPIN);
	ae.scsi_id = cpu_to_be64(tgt->scsi_id);
	ae.wwpn = cpu_to_be64(tgt->wwpn);
	ae.node_name = cpu_to_be64(tgt->ids.node_name);
	ibmvfc_run_async_work(wq, vhost, &ae);

	post[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(rport->fpin_stats.cn_clear);
	post[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(rport->fpin_stats.li_failure_unknown);
	post[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_LINK_CONGESTED],
			post[IBMVFC_AE_FPIN_LINK_CONGESTED]);
	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_PORT_CONGESTED],
			post[IBMVFC_AE_FPIN_PORT_CONGESTED]);
	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_PORT_CLEARED],
			post[IBMVFC_AE_FPIN_PORT_CLEARED]);
	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_PORT_DEGRADED],
			post[IBMVFC_AE_FPIN_PORT_DEGRADED]);
	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED],
			post[IBMVFC_AE_FPIN_CONGESTION_CLEARED]);

	/*
	 * Zero-identifier host-scope path: scsi_id/wwpn/node_name all zero.
	 * LINK_CONGESTED and CONGESTION_CLEARED deliver a host-scope descriptor
	 * (no target needed) and must update fc_host fpin_stats.
	 */
	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	ae.valid = 0x80;
	ae.link_state = IBMVFC_AE_LS_LINK_UP;
	ae.event = cpu_to_be64(IBMVFC_AE_FPIN);
	ae.scsi_id = 0;
	ae.wwpn = 0;
	ae.node_name = 0;

	ae.fpin_status = IBMVFC_AE_FPIN_LINK_CONGESTED;
	ibmvfc_run_async_work(wq, vhost, &ae);

	ae.fpin_status = IBMVFC_AE_FPIN_CONGESTION_CLEARED;
	ibmvfc_run_async_work(wq, vhost, &ae);

	post[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_LINK_CONGESTED],
			pre[IBMVFC_AE_FPIN_LINK_CONGESTED] + 1);
	KUNIT_EXPECT_GE(test, post[IBMVFC_AE_FPIN_CONGESTION_CLEARED],
			pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] + 1);

	/*
	 * Zero-identifier silent-drop path: scsi_id/wwpn/node_name all zero,
	 * but fpin_status is a per-port type (PORT_CONGESTED) that requires a
	 * target. ibmvfc_process_async_work() reaches send_fpin with fpin==NULL
	 * and emits no fc_host_fpin_rcv() call, so no stat counter increments.
	 */
	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	ae.fpin_status = IBMVFC_AE_FPIN_PORT_CONGESTED;
	ibmvfc_run_async_work(wq, vhost, &ae);

	post[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_LINK_CONGESTED],
			post[IBMVFC_AE_FPIN_LINK_CONGESTED]);
	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_PORT_CONGESTED],
			post[IBMVFC_AE_FPIN_PORT_CONGESTED]);
	KUNIT_EXPECT_EQ(test, pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED],
			post[IBMVFC_AE_FPIN_CONGESTION_CLEARED]);

	destroy_workqueue(wq);
	kref_put(&tgt->kref, ibmvfc_release_tgt);
	put_device(&rport->dev);
	scsi_host_put(vhost->host);
}

/**
 * ibmvfc_deregister_channel_guard_test - unit test for ibmvfc_deregister_channel()
 *   guard conditions
 * @test: pointer to kunit structure
 *
 * Tests that ibmvfc_deregister_channel() safely handles a queue that was
 * never registered with the hypervisor (cookie == 0, msgs.handle == NULL):
 * - no H_FREE_SUB_CRQ hypercall is issued
 * - no memset is attempted on a NULL buffer
 * - cookie and cur remain zero after the call
 *
 * Return: void
 */
static void ibmvfc_deregister_channel_guard_test(struct kunit *test)
{
	struct ibmvfc_channels channels = {};
	struct ibmvfc_queue scrq = {};
	struct ibmvfc_host *vhost;

	vhost = ibmvfc_get_first_vhost();
	if (!vhost)
		kunit_skip(test, "No ibmvfc devices available");

	/*
	 * Embed the synthetic queue in a channels struct with one entry.
	 * cookie == 0 and msgs.handle == NULL: ibmvfc_deregister_channel()
	 * must skip both the H_FREE_SUB_CRQ hypercall and the memset.
	 */
	channels.scrqs = &scrq;
	channels.max_queues = 1;

	ibmvfc_deregister_channel(vhost, &channels, 0);

	KUNIT_EXPECT_EQ(test, scrq.cookie, 0UL);
	KUNIT_EXPECT_EQ(test, scrq.cur, 0);
	KUNIT_EXPECT_NULL(test, scrq.msgs.handle);

	scsi_host_put(vhost->host);
}

static struct kunit_case ibmvfc_channel_test_cases[] = {
	KUNIT_CASE(ibmvfc_deregister_channel_guard_test),
	{},
};

static struct kunit_suite ibmvfc_channel_test_suite = {
	.name = "ibmvfc-channel-test",
	.test_cases = ibmvfc_channel_test_cases,
};
kunit_test_init_section_suite(ibmvfc_channel_test_suite);

static struct kunit_case ibmvfc_fpin_test_cases[] = {
	KUNIT_CASE(ibmvfc_async_fpin_test),
	{},
};

static struct kunit_suite ibmvfc_fpin_test_suite = {
	.name = "ibmvfc-fpin-test",
	.test_cases = ibmvfc_fpin_test_cases,
};
kunit_test_init_section_suite(ibmvfc_fpin_test_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dave Marquardt <davemarq@linux.ibm.com>");
MODULE_DESCRIPTION("Test module for IBM Virtual Fibre Channel Driver");
