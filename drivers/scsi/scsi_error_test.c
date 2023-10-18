// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 */
#include <kunit/test.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include "scsi_priv.h"

static struct kunit *kunit_test;

static bool uld_needs_prepare_resubmit(struct scsi_cmnd *cmd)
{
	struct request *rq = scsi_cmd_to_rq(cmd);

	return !rq->q->limits.use_zone_write_lock &&
		blk_rq_is_seq_zoned_write(rq);
}

static void uld_prepare_resubmit(struct list_head *cmd_list)
{
	/* This function must not be called. */
	KUNIT_EXPECT_TRUE(kunit_test, false);
}

/*
 * Verify that .eh_prepare_resubmit() is not called if use_zone_write_lock is
 * true.
 */
static void test_prepare_resubmit1(struct kunit *test)
{
	static struct gendisk disk;
	static struct request_queue q = {
		.disk = &disk,
		.limits = {
			.driver_preserves_write_order = false,
			.use_zone_write_lock = true,
			.zoned = BLK_ZONED_HM,
		}
	};
	static struct scsi_driver uld = {
		.eh_needs_prepare_resubmit = uld_needs_prepare_resubmit,
		.eh_prepare_resubmit = uld_prepare_resubmit,
	};
	static struct scsi_device dev = {
		.request_queue = &q,
		.sdev_gendev.driver = &uld.gendrv,
	};
	static struct rq_and_cmd {
		struct request rq;
		struct scsi_cmnd cmd;
	} cmd1, cmd2;
	LIST_HEAD(cmd_list);

	BUILD_BUG_ON(scsi_cmd_to_rq(&cmd1.cmd) != &cmd1.rq);

	disk.queue = &q;
	cmd1 = (struct rq_and_cmd){
		.rq = {
			.q = &q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 2,
		},
		.cmd.device = &dev,
	};
	cmd2 = cmd1;
	cmd2.rq.__sector = 1;
	list_add_tail(&cmd1.cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd2.cmd.eh_entry, &cmd_list);

	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 2);
	kunit_test = test;
	scsi_call_prepare_resubmit(&cmd_list);
	kunit_test = NULL;
	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 2);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next, &cmd1.cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next, &cmd2.cmd.eh_entry);
}

static struct scsi_driver *uld1, *uld2, *uld3;

static void uld1_prepare_resubmit(struct list_head *cmd_list)
{
	struct scsi_cmnd *cmd;

	KUNIT_EXPECT_EQ(kunit_test, list_count_nodes(cmd_list), 2);
	list_for_each_entry(cmd, cmd_list, eh_entry)
		KUNIT_EXPECT_PTR_EQ(kunit_test, scsi_cmd_to_driver(cmd), uld1);
}

static void uld2_prepare_resubmit(struct list_head *cmd_list)
{
	struct scsi_cmnd *cmd;

	KUNIT_EXPECT_EQ(kunit_test, list_count_nodes(cmd_list), 2);
	list_for_each_entry(cmd, cmd_list, eh_entry)
		KUNIT_EXPECT_PTR_EQ(kunit_test, scsi_cmd_to_driver(cmd), uld2);
}

static void test_prepare_resubmit2(struct kunit *test)
{
	static struct gendisk disk;
	static struct request_queue q = {
		.disk = &disk,
		.limits = {
			.driver_preserves_write_order = true,
			.use_zone_write_lock = false,
			.zoned = BLK_ZONED_HM,
		}
	};
	static struct rq_and_cmd {
		struct request rq;
		struct scsi_cmnd cmd;
	} cmd1, cmd2, cmd3, cmd4, cmd5, cmd6;
	static struct scsi_device dev1, dev2, dev3;
	struct scsi_driver *uld;
	LIST_HEAD(cmd_list);

	BUILD_BUG_ON(scsi_cmd_to_rq(&cmd1.cmd) != &cmd1.rq);

	uld = kzalloc(3 * sizeof(*uld), GFP_KERNEL);
	uld1 = &uld[0];
	uld1->eh_needs_prepare_resubmit = uld_needs_prepare_resubmit;
	uld1->eh_prepare_resubmit = uld1_prepare_resubmit;
	uld2 = &uld[1];
	uld2->eh_needs_prepare_resubmit = uld_needs_prepare_resubmit;
	uld2->eh_prepare_resubmit = uld2_prepare_resubmit;
	uld3 = &uld[2];
	disk.queue = &q;
	dev1.sdev_gendev.driver = &uld1->gendrv;
	dev1.request_queue = &q;
	dev2.sdev_gendev.driver = &uld2->gendrv;
	dev2.request_queue = &q;
	dev3.sdev_gendev.driver = &uld3->gendrv;
	dev3.request_queue = &q;
	cmd1 = (struct rq_and_cmd){
		.rq = {
			.q = &q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 3,
		},
		.cmd.device = &dev1,
	};
	cmd2 = cmd1;
	cmd2.rq.__sector = 4;
	cmd3 = (struct rq_and_cmd){
		.rq = {
			.q = &q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 1,
		},
		.cmd.device = &dev2,
	};
	cmd4 = cmd3;
	cmd4.rq.__sector = 2,
	cmd5 = (struct rq_and_cmd){
		.rq = {
			.q = &q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 5,
		},
		.cmd.device = &dev3,
	};
	cmd6 = cmd5;
	cmd6.rq.__sector = 6;
	list_add_tail(&cmd3.cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd1.cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd2.cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd5.cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd6.cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd4.cmd.eh_entry, &cmd_list);

	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 6);
	kunit_test = test;
	scsi_call_prepare_resubmit(&cmd_list);
	kunit_test = NULL;
	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 6);
	KUNIT_EXPECT_TRUE(test, uld1 < uld2);
	KUNIT_EXPECT_TRUE(test, uld2 < uld3);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next, &cmd1.cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next, &cmd2.cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next,
			    &cmd3.cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next,
			    &cmd4.cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next->next,
			    &cmd5.cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next->next->next,
			    &cmd6.cmd.eh_entry);
	kfree(uld);
}

static struct kunit_case prepare_resubmit_test_cases[] = {
	KUNIT_CASE(test_prepare_resubmit1),
	KUNIT_CASE(test_prepare_resubmit2),
	{}
};

static struct kunit_suite prepare_resubmit_test_suite = {
	.name = "prepare_resubmit",
	.test_cases = prepare_resubmit_test_cases,
};
kunit_test_suite(prepare_resubmit_test_suite);

MODULE_DESCRIPTION("scsi_call_prepare_resubmit() unit tests");
MODULE_AUTHOR("Bart Van Assche");
MODULE_LICENSE("GPL");
