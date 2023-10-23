// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 */
#include <kunit/test.h>
#include <linux/cleanup.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_host.h>
#include "scsi_priv.h"

#define ALLOC(type, ...)					\
	({							\
		type *obj;					\
		obj = kmalloc(sizeof(*obj), GFP_KERNEL);	\
		if (obj)					\
			*obj = (type){ __VA_ARGS__ };		\
		obj;						\
	})

#define ALLOC_Q(...) ALLOC(struct request_queue, __VA_ARGS__)

#define ALLOC_SDEV(...) ALLOC(struct scsi_device, __VA_ARGS__)

#define ALLOC_CMD(...) ALLOC(struct rq_and_cmd, __VA_ARGS__)

static struct kunit *kunit_test;

static void uld_prepare_resubmit(struct list_head *cmd_list)
{
	/* This function must not be called. */
	KUNIT_EXPECT_TRUE(kunit_test, false);
}

/*
 * Verify that .eh_prepare_resubmit() is not called if needs_prepare_resubmit is
 * false.
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
		.eh_prepare_resubmit = uld_prepare_resubmit,
	};
	static const struct scsi_host_template host_template;
	static struct Scsi_Host host = {
		.hostt = &host_template,
	};
	static struct scsi_device dev = {
		.request_queue = &q,
		.sdev_gendev.driver = &uld.gendrv,
		.host = &host,
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
	scsi_call_prepare_resubmit(&host, &cmd_list);
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
	static const struct scsi_host_template host_template = {
		.needs_prepare_resubmit = true,
	};
	static struct Scsi_Host host = {
		.hostt = &host_template,
	};
	struct gendisk *disk __free(kfree) = NULL;
	struct request_queue *q __free(kfree) =
		ALLOC_Q(.limits = {
				.driver_preserves_write_order = true,
				.use_zone_write_lock = false,
				.zoned = BLK_ZONED_HM,
			});
	struct rq_and_cmd {
		struct request rq;
		struct scsi_cmnd cmd;
	} *cmd1 __free(kfree), *cmd2 __free(kfree), *cmd3 __free(kfree),
		*cmd4 __free(kfree), *cmd5 __free(kfree), *cmd6 __free(kfree);
	struct scsi_device *dev1 __free(kfree), *dev2 __free(kfree),
		*dev3 __free(kfree);
	struct scsi_driver *uld __free(kfree);
	LIST_HEAD(cmd_list);

	BUILD_BUG_ON(scsi_cmd_to_rq(&cmd1->cmd) != &cmd1->rq);

	uld = kzalloc(3 * sizeof(*uld), GFP_KERNEL);
	uld1 = &uld[0];
	uld1->eh_prepare_resubmit = uld1_prepare_resubmit;
	uld2 = &uld[1];
	uld2->eh_prepare_resubmit = uld2_prepare_resubmit;
	uld3 = &uld[2];
	disk = kzalloc(sizeof(*disk), GFP_KERNEL);
	disk->queue = q;
	q->disk = disk;
	dev1 = ALLOC_SDEV(.sdev_gendev.driver = &uld1->gendrv,
			  .request_queue = q, .host = &host);
	dev2 = ALLOC_SDEV(.sdev_gendev.driver = &uld2->gendrv,
			  .request_queue = q, .host = &host);
	dev3 = ALLOC_SDEV(.sdev_gendev.driver = &uld3->gendrv,
			  .request_queue = q, .host = &host);
	cmd1 = ALLOC_CMD(
		.rq = {
			.q = q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 3,
		},
		.cmd.device = dev1,
			 );
	cmd2 = ALLOC_CMD();
	*cmd2 = *cmd1;
	cmd2->rq.__sector = 4;
	cmd3 = ALLOC_CMD(
		.rq = {
			.q = q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 1,
		},
		.cmd.device = dev2,
			 );
	cmd4 = kmemdup(cmd3, sizeof(*cmd3), GFP_KERNEL);
	cmd4->rq.__sector = 2,
	cmd5 = ALLOC_CMD(
		.rq = {
			.q = q,
			.cmd_flags = REQ_OP_WRITE,
			.__sector = 5,
		},
		.cmd.device = dev3,
			 );
	cmd6 = kmemdup(cmd5, sizeof(*cmd3), GFP_KERNEL);
	cmd6->rq.__sector = 6;
	list_add_tail(&cmd3->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd1->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd2->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd5->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd6->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd4->cmd.eh_entry, &cmd_list);

	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 6);
	kunit_test = test;
	scsi_call_prepare_resubmit(&host, &cmd_list);
	kunit_test = NULL;
	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 6);
	KUNIT_EXPECT_TRUE(test, uld1 < uld2);
	KUNIT_EXPECT_TRUE(test, uld2 < uld3);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next, &cmd1->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next, &cmd2->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next,
			    &cmd3->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next,
			    &cmd4->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next->next,
			    &cmd5->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next->next->next,
			    &cmd6->cmd.eh_entry);
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
