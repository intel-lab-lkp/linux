// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 */
#include <kunit/test.h>
#include <linux/cleanup.h>
#include <linux/list_sort.h>
#include <linux/slab.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include "sd.h"

#define ALLOC_Q(...)                                                \
	({                                                          \
		struct request_queue *q;                            \
		q = kmalloc(sizeof(*q), GFP_KERNEL);                \
		if (q)                                              \
			*q = (struct request_queue){ __VA_ARGS__ }; \
		q;                                                  \
	})

#define ALLOC_CMD(...)                                             \
	({                                                         \
		struct rq_and_cmd *cmd;                            \
		cmd = kmalloc(sizeof(*cmd), GFP_KERNEL);           \
		if (cmd)                                           \
			*cmd = (struct rq_and_cmd){ __VA_ARGS__ }; \
		cmd;                                               \
	})

struct rq_and_cmd {
	struct request rq;
	struct scsi_cmnd cmd;
};

/*
 * Verify that sd_cmp_sector() does what it is expected to do.
 */
static void test_sd_cmp_sector(struct kunit *test)
{
	struct request_queue *q1 __free(kfree) =
		ALLOC_Q(.limits.use_zone_write_lock = true);
	struct request_queue *q2 __free(kfree) =
		ALLOC_Q(.limits.use_zone_write_lock = false);
	struct rq_and_cmd *cmd1 __free(kfree) = ALLOC_CMD(.rq = {
								  .q = q1,
								  .__sector = 7,
							  });
	struct rq_and_cmd *cmd2 __free(kfree) = ALLOC_CMD(.rq = {
								  .q = q1,
								  .__sector = 5,
							  });
	struct rq_and_cmd *cmd3 __free(kfree) = ALLOC_CMD(.rq = {
								  .q = q2,
								  .__sector = 7,
							  });
	struct rq_and_cmd *cmd4 __free(kfree) = ALLOC_CMD(.rq = {
								  .q = q2,
								  .__sector = 5,
							  });
	LIST_HEAD(cmd_list);

	list_add_tail(&cmd1->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd2->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd3->cmd.eh_entry, &cmd_list);
	list_add_tail(&cmd4->cmd.eh_entry, &cmd_list);
	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 4);
	list_sort(NULL, &cmd_list, sd_cmp_sector);
	KUNIT_EXPECT_EQ(test, list_count_nodes(&cmd_list), 4);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next, &cmd4->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next, &cmd3->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next,
			    &cmd1->cmd.eh_entry);
	KUNIT_EXPECT_PTR_EQ(test, cmd_list.next->next->next->next,
			    &cmd2->cmd.eh_entry);
}

static struct kunit_case sd_test_cases[] = {
	KUNIT_CASE(test_sd_cmp_sector),
	{}
};

static struct kunit_suite sd_test_suite = {
	.name = "sd",
	.test_cases = sd_test_cases,
};
kunit_test_suite(sd_test_suite);

MODULE_DESCRIPTION("SCSI disk (sd) driver unit tests");
MODULE_AUTHOR("Bart Van Assche");
MODULE_LICENSE("GPL");
