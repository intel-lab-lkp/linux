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
 * ibmvfc_async_fpin_event_test - unit test for IBMVFC_AE_FPIN parts of
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
	enum ibmvfc_ae_fpin_status fs;
	struct fc_host_attrs *fc_host;
	struct ibmvfc_async_crq crq[IBMVFC_AE_FPIN_CONGESTION_CLEARED + 1];
	struct ibmvfc_target *tgt;
	struct ibmvfc_host *vhost;
	struct list_head *queue;
	struct list_head *headp;

	headp = ibmvfc_get_headp();
	if (list_empty(headp))
		kunit_skip(test, "No ibmvfc devices available");
	queue = headp->next;
	vhost = container_of(queue, struct ibmvfc_host, queue);

	KUNIT_ASSERT_GE_MSG(test, vhost->num_targets, 1, "No targets");
	tgt = list_first_entry(&vhost->targets, struct ibmvfc_target, queue);
	KUNIT_EXPECT_NOT_NULL(test, tgt->rport);

	fc_host = shost_to_fc_host(vhost->host);

	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(tgt->rport->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(tgt->rport->fpin_stats.cn_clear);
	pre[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(tgt->rport->fpin_stats.li_failure_unknown);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	for (fs = IBMVFC_AE_FPIN_LINK_CONGESTED; fs <= IBMVFC_AE_FPIN_CONGESTION_CLEARED; fs++) {
		crq[fs].valid = 0x80;
		crq[fs].link_state = IBMVFC_AE_LS_LINK_UP;
		crq[fs].fpin_status = fs;
		crq[fs].event = cpu_to_be64(IBMVFC_AE_FPIN);
		crq[fs].scsi_id = cpu_to_be64(tgt->scsi_id);
		crq[fs].wwpn = cpu_to_be64(tgt->wwpn);
		crq[fs].node_name = cpu_to_be64(tgt->ids.node_name);
		ibmvfc_handle_async(&crq[fs], vhost, false);
		msleep(1U);
	}

	post[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(tgt->rport->fpin_stats.cn);
	post[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(tgt->rport->fpin_stats.cn_clear);
	post[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(tgt->rport->fpin_stats.li_failure_unknown);
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

	pre[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	pre[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(tgt->rport->fpin_stats.cn);
	pre[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(tgt->rport->fpin_stats.cn_clear);
	pre[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(tgt->rport->fpin_stats.li_failure_unknown);
	pre[IBMVFC_AE_FPIN_CONGESTION_CLEARED] = READ_ONCE(fc_host->fpin_stats.cn_clear);

	/* bad path */
	crq[0].valid = 0x80;
	crq[0].link_state = IBMVFC_AE_LS_LINK_UP;
	crq[0].fpin_status = 0; /* bad value */
	crq[0].event = cpu_to_be64(IBMVFC_AE_FPIN);
	crq[0].scsi_id = cpu_to_be64(tgt->scsi_id);
	crq[0].wwpn = cpu_to_be64(tgt->wwpn);
	crq[0].node_name = cpu_to_be64(tgt->ids.node_name);
	ibmvfc_handle_async(&crq[0], vhost, false);
	msleep(1U);

	post[IBMVFC_AE_FPIN_LINK_CONGESTED] = READ_ONCE(fc_host->fpin_stats.cn_device_specific);
	post[IBMVFC_AE_FPIN_PORT_CONGESTED] = READ_ONCE(tgt->rport->fpin_stats.cn);
	post[IBMVFC_AE_FPIN_PORT_CLEARED] = READ_ONCE(tgt->rport->fpin_stats.cn_clear);
	post[IBMVFC_AE_FPIN_PORT_DEGRADED] = READ_ONCE(tgt->rport->fpin_stats.li_failure_unknown);
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
}

#define IBMVFC_TEST_FPIN_EXT(fs, ev, stat, crq) {		\
	crq.valid = 0x80;					\
	crq.flags = IBMVFC_ASYNC_IS_FPIN_EXT;			\
	crq.link_state = IBMVFC_AE_LS_LINK_UP;			\
	crq.fpin_status = (fs);					\
	crq.event = cpu_to_be16(IBMVFC_AE_FPIN);		\
	crq.wwpn = cpu_to_be64(tgt->wwpn);			\
	crq.fpin_data.flags = IBMVFC_FPIN_EVENT_TYPE_VALID;	\
	crq.fpin_data.event_type = cpu_to_be16((ev));		\
	pre = READ_ONCE(tgt->rport->fpin_stats.stat);		\
	ibmvfc_handle_async((struct ibmvfc_crq *)&crq, vhost, true);	\
	msleep(1U);							\
	post = READ_ONCE(tgt->rport->fpin_stats.stat);		\
}

/**
 * ibmvfc_extended_fpin_test - unit test for extended FPIN events
 * @test: pointer to kunit structure
 *
 * Tests
 *
 * Return: void
 */
static void ibmvfc_extended_fpin_test(struct kunit *test)
{
	enum ibmvfc_ae_fpin_status fs;
	struct ibmvfc_async_subq_fpin crq[IBMVFC_AE_FPIN_CONGESTION_CLEARED+1] = {};
	struct ibmvfc_async_subq_fpin
		crqcn[IBMVFC_AE_FPIN_PORT_CONGESTED][FPIN_CONGN_DEVICE_SPEC+1] = {};
	struct ibmvfc_async_subq_fpin crqportdg[FPIN_LI_DEVICE_SPEC+1] = {};
	struct ibmvfc_target *tgt;
	struct ibmvfc_host *vhost;
	struct list_head *headp;
	LIST_HEAD(evt_doneq);
	u64 pre, post;

	headp = ibmvfc_get_headp();
	KUNIT_ASSERT_FALSE_MSG(test, list_empty(headp), "No ibmvfc devices available\n");
	vhost = list_first_entry(headp, struct ibmvfc_host, queue);
	KUNIT_ASSERT_GE_MSG(test, vhost->num_targets, 1, "No targets");

	tgt = list_first_entry(&vhost->targets, struct ibmvfc_target, queue);
	KUNIT_ASSERT_NOT_NULL(test, tgt->rport);

	for (fs = IBMVFC_AE_FPIN_LINK_CONGESTED; fs <= IBMVFC_AE_FPIN_CONGESTION_CLEARED; fs++) {
		switch (fs) {
		case IBMVFC_AE_FPIN_PORT_CLEARED:
		case IBMVFC_AE_FPIN_CONGESTION_CLEARED:
			crq[fs].valid = 0x80;
			crq[fs].flags = IBMVFC_ASYNC_IS_FPIN_EXT;
			crq[fs].link_state = IBMVFC_AE_LS_LINK_UP;
			crq[fs].fpin_status = fs;
			crq[fs].event = cpu_to_be16(IBMVFC_AE_FPIN);
			crq[fs].wwpn = cpu_to_be64(tgt->wwpn);
			crq[fs].fpin_data.flags = IBMVFC_FPIN_EVENT_TYPE_VALID;
			crq[fs].fpin_data.event_type = cpu_to_be16(FPIN_CONGN_CLEAR);
			pre = READ_ONCE(tgt->rport->fpin_stats.cn_clear);
			ibmvfc_handle_async((struct ibmvfc_crq *)&crq[fs], vhost, true);
			msleep(1U);
			post = READ_ONCE(tgt->rport->fpin_stats.cn_clear);
			break;
		case IBMVFC_AE_FPIN_LINK_CONGESTED:
		case IBMVFC_AE_FPIN_PORT_CONGESTED:
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_CONGN_CLEAR, cn_clear,
					     crqcn[fs-1][FPIN_CONGN_CLEAR]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_CONGN_LOST_CREDIT,
					     cn_lost_credit,
					     crqcn[fs-1][FPIN_CONGN_LOST_CREDIT]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_CONGN_CREDIT_STALL,
					     cn_credit_stall,
					     crqcn[fs-1][FPIN_CONGN_CREDIT_STALL]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_CONGN_OVERSUBSCRIPTION,
					     cn_oversubscription,
					     crqcn[fs-1][FPIN_CONGN_OVERSUBSCRIPTION]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_CONGN_DEVICE_SPEC,
					     cn_device_specific,
					     crqcn[fs-1][FPIN_CONGN_DEVICE_SPEC]);
			break;
		case IBMVFC_AE_FPIN_PORT_DEGRADED:
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_UNKNOWN,
					     li_failure_unknown,
					     crqportdg[FPIN_LI_UNKNOWN]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_LINK_FAILURE,
					     li_link_failure_count,
					     crqportdg[FPIN_LI_LINK_FAILURE]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_LOSS_OF_SYNC,
					     li_loss_of_sync_count,
					     crqportdg[FPIN_LI_LOSS_OF_SYNC]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_LOSS_OF_SIG,
					     li_loss_of_signals_count,
					     crqportdg[FPIN_LI_LOSS_OF_SIG]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_PRIM_SEQ_ERR,
					     li_prim_seq_err_count,
					     crqportdg[FPIN_LI_PRIM_SEQ_ERR]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_INVALID_TX_WD,
					     li_invalid_tx_word_count,
					     crqportdg[FPIN_LI_INVALID_TX_WD]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_INVALID_CRC,
					     li_invalid_crc_count,
					     crqportdg[FPIN_LI_INVALID_CRC]);
			IBMVFC_TEST_FPIN_EXT(fs, FPIN_LI_DEVICE_SPEC,
					     li_device_specific,
					     crqportdg[FPIN_LI_DEVICE_SPEC]);
			break;
		}
	}
}

static struct kunit_case ibmvfc_fpin_test_cases[] = {
	KUNIT_CASE(ibmvfc_async_fpin_test),
	KUNIT_CASE(ibmvfc_extended_fpin_test),
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
