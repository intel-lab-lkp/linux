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
 * ibmvfc_async_fpin_test - unit test for IBMVFC_AE_FPIN parts of
 * ibmvfc_handle_async
 * @test: pointer to kunit structure
 *
 * Tests
 * - error returns from ibmvfc_handle_async
 * - statistics updates
 *
 * Return: void
 */
static void ibmvfc_async_fpin_test(struct kunit *test)
{
	u64 post[IBMVFC_AE_FPIN_CONGESTION_CLEARED + 1];
	u64 pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED + 1];
	struct ibmvfc_async_crq ae[IBMVFC_AE_FPIN_CONGESTION_CLEARED + 1];
	enum ibmvfc_ae_fpin_status fs;
	struct fc_host_attrs *fc_host;
	struct ibmvfc_target *tgt;
	struct ibmvfc_host *vhost;
	struct fc_rport *rport;
	unsigned long flags;

	vhost = ibmvfc_get_first_vhost();
	if (!vhost)
		kunit_skip(test, "No ibmvfc devices available");

	spin_lock_irqsave(vhost->host->host_lock, flags);
	if (vhost->scsi_scrqs.num_targets < 1) {
		spin_unlock_irqrestore(vhost->host->host_lock, flags);
		scsi_host_put(vhost->host);
		kunit_skip(test, "No targets");
	}
	tgt = list_first_entry(&vhost->scsi_scrqs.targets, struct ibmvfc_target, queue);
	if (!tgt->rport) {
		spin_unlock_irqrestore(vhost->host->host_lock, flags);
		scsi_host_put(vhost->host);
		kunit_skip(test, "No rport");
	}
	rport = tgt->rport;
	get_device(&rport->dev);
	kref_get(&tgt->kref);
	spin_unlock_irqrestore(vhost->host->host_lock, flags);

	fc_host = shost_to_fc_host(vhost->host);

	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(rport->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(rport->fpin_stats.cn_clear);
	pre[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(rport->fpin_stats.li_failure_unknown);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	for (fs = IBMVFC_AE_FPIN_LINK_CONGESTED; fs <= IBMVFC_AE_FPIN_CONGESTION_CLEARED; fs++) {
		ae[fs].valid = 0x80;
		ae[fs].link_state = IBMVFC_AE_LS_LINK_UP;
		ae[fs].fpin_status = fs;
		ae[fs].event = cpu_to_be64(IBMVFC_AE_FPIN);
		ae[fs].scsi_id = cpu_to_be64(tgt->scsi_id);
		ae[fs].wwpn = cpu_to_be64(tgt->wwpn);
		ae[fs].node_name = cpu_to_be64(tgt->ids.node_name);
		spin_lock_irqsave(vhost->host->host_lock, flags);
		ibmvfc_handle_async(&ae[fs], vhost);
		spin_unlock_irqrestore(vhost->host->host_lock, flags);
		ae[fs].valid = 0;
		wmb(); /* ensure valid bit clear is visible before checking stats */
	}
	flush_workqueue(vhost->fpin_workq);

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

	ae[0].valid = 0x80;
	ae[0].link_state = IBMVFC_AE_LS_LINK_UP;
	ae[0].fpin_status = 0; /* bad value */
	ae[0].event = cpu_to_be64(IBMVFC_AE_FPIN);
	ae[0].scsi_id = cpu_to_be64(tgt->scsi_id);
	ae[0].wwpn = cpu_to_be64(tgt->wwpn);
	ae[0].node_name = cpu_to_be64(tgt->ids.node_name);
	spin_lock_irqsave(vhost->host->host_lock, flags);
	ibmvfc_handle_async(&ae[0], vhost);
	spin_unlock_irqrestore(vhost->host->host_lock, flags);
	ae[0].valid = 0;
	wmb(); /* ensure valid bit clear is visible before checking stats */
	flush_workqueue(vhost->fpin_workq);

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

	kref_put(&tgt->kref, ibmvfc_release_tgt);
	put_device(&rport->dev);
	scsi_host_put(vhost->host);
}

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
