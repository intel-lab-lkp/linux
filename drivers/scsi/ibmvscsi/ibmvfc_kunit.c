// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_transport_fc.h>
#include "ibmvfc.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dave Marquardt <davemarq@linux.ibm.com>");
MODULE_DESCRIPTION("Test module for IBM Virtual Fibre Channel Driver");
