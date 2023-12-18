// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 */
#include <kunit/test.h>
#include <linux/cleanup.h>

static void test_ioprio(struct kunit *test)
{
	static struct block_device bdev;
	static struct gendisk disk = { .part0 = &bdev };
	static struct request_queue queue = { .disk = &disk };
	static struct blk_mq_hw_ctx hctx = { .queue = &queue };
	static struct bio bio1 = { .bi_bdev = &bdev,
				   .bi_opf = REQ_OP_WRITE,
				   .bi_ioprio = IOPRIO_CLASS_IDLE
						<< IOPRIO_CLASS_SHIFT };
	static struct request rq1 = { .q = &queue,
				      .cmd_flags = REQ_OP_WRITE,
				      .__sector = 1,
				      .__data_len = 1,
				      .bio = &bio1,
				      .mq_hctx = &hctx,
				      .ioprio = IOPRIO_CLASS_IDLE
						<< IOPRIO_CLASS_SHIFT };
	static struct bio bio2 = { .bi_bdev = &bdev,
				   .bi_opf = REQ_OP_WRITE,
				   .bi_ioprio = IOPRIO_CLASS_BE
						<< IOPRIO_CLASS_SHIFT };
	static struct request rq2 = { .q = &queue,
				      .cmd_flags = REQ_OP_WRITE,
				      .__sector = 3,
				      .__data_len = 1,
				      .bio = &bio2,
				      .mq_hctx = &hctx,
				      .ioprio = IOPRIO_CLASS_BE
						<< IOPRIO_CLASS_SHIFT };
	static struct bio bio3 = { .bi_bdev = &bdev,
				   .bi_opf = REQ_OP_WRITE,
				   .bi_ioprio = IOPRIO_CLASS_RT
						<< IOPRIO_CLASS_SHIFT };
	static struct request rq3 = { .q = &queue,
				      .cmd_flags = REQ_OP_WRITE,
				      .__sector = 5,
				      .__data_len = 1,
				      .bio = &bio3,
				      .mq_hctx = &hctx,
				      .ioprio = IOPRIO_CLASS_RT
						<< IOPRIO_CLASS_SHIFT };
	struct request *rq;
	static LIST_HEAD(rq_list);

	bdev.bd_disk = &disk;
	bdev.bd_queue = &queue;
	disk.queue = &queue;

	dd_init_sched(&queue, &mq_deadline);
	dd_prepare_request(&rq1);
	dd_prepare_request(&rq2);
	dd_prepare_request(&rq3);
	list_add_tail(&rq1.queuelist, &rq_list);
	list_add_tail(&rq2.queuelist, &rq_list);
	list_add_tail(&rq3.queuelist, &rq_list);
	dd_insert_requests(&hctx, &rq_list, false);
	rq = dd_dispatch_request(&hctx);
	KUNIT_EXPECT_PTR_EQ(test, rq, &rq3);
	dd_finish_request(rq);
	rq = dd_dispatch_request(&hctx);
	KUNIT_EXPECT_PTR_EQ(test, rq, &rq2);
	dd_finish_request(rq);
	rq = dd_dispatch_request(&hctx);
	KUNIT_EXPECT_PTR_EQ(test, rq, &rq1);
	dd_finish_request(rq);
	dd_exit_sched(queue.elevator);
}

/*
 * Test that the write order is preserved if a higher I/O priority is assigned
 * to higher LBAs. This test fails if dd_zone_prio() always returns
 * DD_INVALID_PRIO.
 */
static void test_zone_prio(struct kunit *test)
{
	static struct block_device bdev;
	static unsigned long seq_zones_wlock[1];
	static struct gendisk disk = { .conv_zones_bitmap = NULL,
				       .seq_zones_wlock = seq_zones_wlock,
				       .part0 = &bdev };
	static struct request_queue queue = {
		.disk = &disk,
		.limits = { .zoned = BLK_ZONED_HM, .chunk_sectors = 16 }
	};
	static struct blk_mq_hw_ctx hctx = { .queue = &queue };
	static struct bio bio1 = { .bi_bdev = &bdev,
				   .bi_opf = REQ_OP_WRITE,
				   .bi_ioprio = IOPRIO_CLASS_IDLE
						<< IOPRIO_CLASS_SHIFT };
	static struct request rq1 = { .q = &queue,
				      .cmd_flags = REQ_OP_WRITE,
				      .__sector = 1,
				      .__data_len = 1,
				      .bio = &bio1,
				      .mq_hctx = &hctx,
				      .ioprio = IOPRIO_CLASS_IDLE
						<< IOPRIO_CLASS_SHIFT };
	static struct bio bio2 = { .bi_bdev = &bdev,
				   .bi_opf = REQ_OP_WRITE,
				   .bi_ioprio = IOPRIO_CLASS_BE
						<< IOPRIO_CLASS_SHIFT };
	static struct request rq2 = { .q = &queue,
				      .cmd_flags = REQ_OP_WRITE,
				      .__sector = 3,
				      .__data_len = 1,
				      .bio = &bio2,
				      .mq_hctx = &hctx,
				      .ioprio = IOPRIO_CLASS_BE
						<< IOPRIO_CLASS_SHIFT };
	static struct bio bio3 = { .bi_bdev = &bdev,
				   .bi_opf = REQ_OP_WRITE,
				   .bi_ioprio = IOPRIO_CLASS_RT
						<< IOPRIO_CLASS_SHIFT };
	static struct request rq3 = { .q = &queue,
				      .cmd_flags = REQ_OP_WRITE,
				      .__sector = 5,
				      .__data_len = 1,
				      .bio = &bio3,
				      .mq_hctx = &hctx,
				      .ioprio = IOPRIO_CLASS_RT
						<< IOPRIO_CLASS_SHIFT };
	struct request *rq;
	static LIST_HEAD(rq_list);

	bdev.bd_disk = &disk;
	bdev.bd_queue = &queue;
	disk.queue = &queue;

	KUNIT_EXPECT_TRUE(test, blk_rq_is_seq_zoned_write(&rq1));
	KUNIT_EXPECT_TRUE(test, blk_rq_is_seq_zoned_write(&rq2));
	KUNIT_EXPECT_TRUE(test, blk_rq_is_seq_zoned_write(&rq3));

	dd_init_sched(&queue, &mq_deadline);
	dd_prepare_request(&rq1);
	dd_prepare_request(&rq2);
	dd_prepare_request(&rq3);
	list_add_tail(&rq1.queuelist, &rq_list);
	list_add_tail(&rq2.queuelist, &rq_list);
	list_add_tail(&rq3.queuelist, &rq_list);
	dd_insert_requests(&hctx, &rq_list, false);
	rq = dd_dispatch_request(&hctx);
	KUNIT_EXPECT_PTR_EQ(test, rq, &rq1);
	dd_finish_request(rq);
	rq = dd_dispatch_request(&hctx);
	KUNIT_EXPECT_PTR_EQ(test, rq, &rq2);
	dd_finish_request(rq);
	rq = dd_dispatch_request(&hctx);
	KUNIT_EXPECT_PTR_EQ(test, rq, &rq3);
	dd_finish_request(rq);
	dd_exit_sched(queue.elevator);
}

static struct kunit_case mq_deadline_test_cases[] = {
	KUNIT_CASE(test_ioprio),
	KUNIT_CASE(test_zone_prio),
	{}
};

static struct kunit_suite mq_deadline_test_suite = {
	.name = "mq-deadline",
	.test_cases = mq_deadline_test_cases,
};
kunit_test_suite(mq_deadline_test_suite);

MODULE_DESCRIPTION("mq-deadline unit tests");
MODULE_AUTHOR("Bart Van Assche");
MODULE_LICENSE("GPL");
