// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for scsi_logging.c.
 *
 * Copyright 2026 Google LLC
 */
#include <kunit/test.h>
#include <linux/blkdev.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_proto.h>

#define MAX_CAPTURED_LINES 16
#define MAX_LINE_LEN 256

struct captured_dev_printk {
	const char *level;
	const struct device *dev;
	char msg[MAX_LINE_LEN];
};

static struct captured_dev_printk captured_logs[MAX_CAPTURED_LINES];
static int captured_count;

static void test_capture_dev_printk(const char *level, const struct device *dev,
				    const char *fmt, va_list args)
{
	if (captured_count < MAX_CAPTURED_LINES) {
		captured_logs[captured_count].level = level;
		captured_logs[captured_count].dev = dev;
		vscnprintf(captured_logs[captured_count].msg,
			   sizeof(captured_logs[captured_count].msg), fmt,
			   args);
		captured_count++;
	}
}

static void scsi_logging_test_reset(void)
{
	captured_count = 0;
	memset(captured_logs, 0, sizeof(captured_logs));
}

static int scsi_logging_test_init(struct kunit *test)
{
	scsi_logging_test_reset();
	scsi_logging_test_dev_printk = test_capture_dev_printk;
	return 0;
}

static void scsi_logging_test_exit(struct kunit *test)
{
	scsi_logging_test_dev_printk = NULL;
}

struct test_scsi_cmd {
	struct request rq;
	struct scsi_cmnd cmd;
};

struct test_fixture {
	struct scsi_device *sdev;
	struct gendisk *disk;
	struct request_queue *q;
	struct test_scsi_cmd *tscmd;
};

static struct test_fixture *scsi_logging_create_fixture(struct kunit *test)
{
	struct test_fixture *tf = kunit_kzalloc(test, sizeof(*tf), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, tf);

	tf->sdev = kunit_kzalloc(test, sizeof(*tf->sdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tf->sdev);

	tf->disk = kunit_kzalloc(test, sizeof(*tf->disk), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tf->disk);

	tf->q = kunit_kzalloc(test, sizeof(*tf->q), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tf->q);

	tf->tscmd = kunit_kzalloc(test, sizeof(*tf->tscmd), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tf->tscmd);

	tf->q->disk = tf->disk;
	tf->tscmd->cmd.device = tf->sdev;
	strscpy(tf->disk->disk_name, "sda", sizeof(tf->disk->disk_name));

	return tf;
}

static void test_sdev_prefix_printk(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);

	/* NULL sdev should produce no output */
	sdev_prefix_printk(KERN_INFO, NULL, "test", "should not print");
	KUNIT_EXPECT_EQ(test, captured_count, 0);

	/* Without name prefix */
	sdev_prefix_printk(KERN_WARNING, tf->sdev, NULL, "warning %d", 42);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].level, KERN_WARNING);
	KUNIT_EXPECT_PTR_EQ(test, captured_logs[0].dev, &tf->sdev->sdev_gendev);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg, "warning 42");

	/* With name prefix */
	scsi_logging_test_reset();
	sdev_prefix_printk(KERN_ERR, tf->sdev, "adapter0", "error code %d", -5);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].level, KERN_ERR);
	KUNIT_EXPECT_PTR_EQ(test, captured_logs[0].dev, &tf->sdev->sdev_gendev);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[adapter0] error code -5");
}

static void test_scmd_printk(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);

	/* NULL scmd should produce no output */
	scmd_printk(KERN_INFO, NULL, "should not print");
	KUNIT_EXPECT_EQ(test, captured_count, 0);

	/* scmd with tag but no disk name */
	tf->tscmd->rq.q = NULL;
	tf->tscmd->rq.tag = 5;
	scmd_printk(KERN_INFO, &tf->tscmd->cmd, "test msg %d", 10);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].level, KERN_INFO);
	KUNIT_EXPECT_PTR_EQ(test, captured_logs[0].dev, &tf->sdev->sdev_gendev);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg, "tag#5 test msg 10");

	/* scmd with disk name and tag */
	scsi_logging_test_reset();
	tf->tscmd->rq.q = tf->q;
	tf->tscmd->rq.tag = 12;
	scmd_printk(KERN_ERR, &tf->tscmd->cmd, "failed status");
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].level, KERN_ERR);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[sda] tag#12 failed status");

	/* scmd with disk name but no tag (tag < 0) */
	scsi_logging_test_reset();
	tf->tscmd->rq.tag = -1;
	scmd_printk(KERN_NOTICE, &tf->tscmd->cmd, "no tag notification");
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[sda] no tag notification");
}

static void test_scsi_print_command(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);
	static const unsigned char cdb6[6] = { 0x00, 0x00, 0x00,
					       0x00, 0x00, 0x00 };
	static const unsigned char cdb10[10] = { 0x28, 0x00, 0x00, 0x00, 0x00,
						 0x00, 0x00, 0x00, 0x08, 0x00 };
	static const unsigned char cdb16[16] = { 0x88, 0x00, 0x00, 0x00,
						 0x00, 0x00, 0x00, 0x00,
						 0x00, 0x00, 0x00, 0x00,
						 0x00, 0x00, 0x08, 0x00 };
	static const unsigned char cdb32[32] = {
		0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
		0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00
	};
	static const unsigned char cdb_vendor[6] = { 0xc0, 0x00, 0x00,
						     0x00, 0x00, 0x00 };
	static const unsigned char cdb_reserved[6] = { 0x60, 0x00, 0x00,
						       0x00, 0x00, 0x00 };

	tf->tscmd->rq.q = tf->q;
	tf->tscmd->rq.tag = 1;

	/* 6-byte TEST UNIT READY */
	memcpy(tf->tscmd->cmd.cmnd, cdb6, sizeof(cdb6));
	tf->tscmd->cmd.cmd_len = sizeof(cdb6);
	scsi_print_command(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#1 CDB: Test Unit Ready 00 00 00 00 00 00");

	/* 10-byte READ(10) */
	scsi_logging_test_reset();
	memcpy(tf->tscmd->cmd.cmnd, cdb10, sizeof(cdb10));
	tf->tscmd->cmd.cmd_len = sizeof(cdb10);
	scsi_print_command(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#1 CDB: Read(10) 28 00 00 00 00 00 00 00 08 00");

	/* 16-byte READ(16) */
	scsi_logging_test_reset();
	memcpy(tf->tscmd->cmd.cmnd, cdb16, sizeof(cdb16));
	tf->tscmd->cmd.cmd_len = sizeof(cdb16);
	scsi_print_command(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#1 CDB: Read(16) 88 00 00 00 00 00 00 00 00 00 00 00 00 00 08 00");

	/* 32-byte CDB (CDB len > 16 generates multiple printk lines) */
	scsi_logging_test_reset();
	memcpy(tf->tscmd->cmd.cmnd, cdb32, sizeof(cdb32));
	tf->tscmd->cmd.cmd_len = sizeof(cdb32);
	scsi_print_command(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 3);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[sda] tag#1 CDB: Read(32)\n");
	KUNIT_EXPECT_STREQ(
		test, captured_logs[1].msg,
		"[sda] tag#1 CDB[00]: 7f 00 00 00 00 00 00 18 00 09 00 00 00 00 00 00");
	KUNIT_EXPECT_STREQ(
		test, captured_logs[2].msg,
		"[sda] tag#1 CDB[10]: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 08 00");

	/* Vendor-specific opcode */
	scsi_logging_test_reset();
	memcpy(tf->tscmd->cmd.cmnd, cdb_vendor, sizeof(cdb_vendor));
	tf->tscmd->cmd.cmd_len = sizeof(cdb_vendor);
	scsi_print_command(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#1 CDB: opcode=0xc0 (vendor) c0 00 00 00 00 00");

	/* Reserved opcode */
	scsi_logging_test_reset();
	memcpy(tf->tscmd->cmd.cmnd, cdb_reserved, sizeof(cdb_reserved));
	tf->tscmd->cmd.cmd_len = sizeof(cdb_reserved);
	scsi_print_command(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#1 CDB: opcode=0x60 (reserved) 60 00 00 00 00 00");
}

static void test_scsi_print_sense_hdr(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);
	struct scsi_sense_hdr sshdr = {
		.response_code = 0x70,
		.sense_key = ILLEGAL_REQUEST,
		.asc = 0x20,
		.ascq = 0x00,
	};

	scsi_print_sense_hdr(tf->sdev, "sda", &sshdr);
	KUNIT_EXPECT_EQ(test, captured_count, 2);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[sda] Sense Key : Illegal Request [current] ");
	KUNIT_EXPECT_STREQ(test, captured_logs[1].msg,
			   "[sda] Add. Sense: Invalid command operation code");

	/* Deferred and descriptor format */
	scsi_logging_test_reset();
	sshdr.response_code = 0x73;
	sshdr.sense_key = UNIT_ATTENTION;
	sshdr.asc = 0x29;
	sshdr.ascq = 0x00;
	scsi_print_sense_hdr(tf->sdev, "sdb", &sshdr);
	KUNIT_EXPECT_EQ(test, captured_count, 2);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sdb] Sense Key : Unit Attention [deferred] [descriptor] ");
	KUNIT_EXPECT_STREQ(
		test, captured_logs[1].msg,
		"[sdb] Add. Sense: Power on, reset, or bus device reset occurred");

	/* Additional sense: Invalid token operation, remote rod token creation not supported */
	scsi_logging_test_reset();
	sshdr.response_code = 0x70;
	sshdr.sense_key = ILLEGAL_REQUEST;
	sshdr.asc = 0x23;
	sshdr.ascq = 0x03;
	scsi_print_sense_hdr(tf->sdev, "sdc", &sshdr);
	KUNIT_EXPECT_EQ(test, captured_count, 2);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sdc] Sense Key : Illegal Request [current] ");
	KUNIT_EXPECT_STREQ(
		test, captured_logs[1].msg,
		"[sdc] Add. Sense: Invalid token operation, remote rod token creation not supported");
}

static void test_scsi_print_sense_buffer(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);
	unsigned char normalized_sense[18] = {
		[0] = 0x70, [2] = NOT_READY, [7] = 10, [12] = 0x04, [13] = 0x01,
	};
	unsigned char raw_sense[16] = { 0 };

	/* Normalized sense buffer */
	__scsi_print_sense(tf->sdev, "sda", normalized_sense,
			   sizeof(normalized_sense));
	KUNIT_EXPECT_EQ(test, captured_count, 2);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[sda] Sense Key : Not Ready [current] ");
	KUNIT_EXPECT_STREQ(
		test, captured_logs[1].msg,
		"[sda] Add. Sense: Logical unit is in process of becoming ready");

	/* Unnormalized / raw sense buffer dumped in hex */
	scsi_logging_test_reset();
	__scsi_print_sense(tf->sdev, "sda", raw_sense, sizeof(raw_sense));
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00");
}

static void test_scsi_print_sense_cmd(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);
	unsigned char *sense_buffer =
		kunit_kzalloc(test, SCSI_SENSE_BUFFERSIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sense_buffer);
	sense_buffer[0] = 0x70;
	sense_buffer[2] = UNIT_ATTENTION;
	sense_buffer[7] = 10;
	sense_buffer[12] = 0x28;
	sense_buffer[13] = 0x00;

	tf->tscmd->cmd.sense_buffer = sense_buffer;
	tf->tscmd->rq.q = tf->q;
	tf->tscmd->rq.tag = 3;

	scsi_print_sense(&tf->tscmd->cmd);
	KUNIT_EXPECT_EQ(test, captured_count, 2);
	KUNIT_EXPECT_STREQ(test, captured_logs[0].msg,
			   "[sda] tag#3 Sense Key : Unit Attention [current] ");
	KUNIT_EXPECT_STREQ(
		test, captured_logs[1].msg,
		"[sda] tag#3 Add. Sense: Not ready to ready change, medium may have changed");
}

static void test_scsi_print_result(struct kunit *test)
{
	struct test_fixture *tf = scsi_logging_create_fixture(test);

	tf->tscmd->rq.q = tf->q;
	tf->tscmd->rq.tag = 4;
	tf->tscmd->cmd.result = (DID_OK << 16) | SAM_STAT_CHECK_CONDITION;
	tf->tscmd->cmd.jiffies_at_alloc = jiffies - 5 * HZ;

	/* Result with message */
	scsi_print_result(&tf->tscmd->cmd, "Failed command", FAILED);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#4 Failed command: FAILED Result: hostbyte=DID_OK driverbyte=DRIVER_OK cmd_age=5s");

	/* Result without message */
	scsi_logging_test_reset();
	scsi_print_result(&tf->tscmd->cmd, NULL, SUCCESS);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#4 SUCCESS Result: hostbyte=DID_OK driverbyte=DRIVER_OK cmd_age=5s");

	/* Result with DID_TRANSPORT_DISRUPTED hostbyte */
	scsi_logging_test_reset();
	tf->tscmd->cmd.result = (DID_TRANSPORT_DISRUPTED << 16) | SAM_STAT_CHECK_CONDITION;
	scsi_print_result(&tf->tscmd->cmd, "Transport disrupted", NEEDS_RETRY);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#4 Transport disrupted: NEEDS_RETRY Result: hostbyte=DID_TRANSPORT_DISRUPTED driverbyte=DRIVER_OK cmd_age=5s");

	/* Result with hostbyte without known string */
	scsi_logging_test_reset();
	tf->tscmd->cmd.result = (0x1f << 16);
	scsi_print_result(&tf->tscmd->cmd, "Unknown hostbyte", FAILED);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#4 Unknown hostbyte: FAILED Result: hostbyte=0x1f driverbyte=DRIVER_OK cmd_age=5s");

	/* Result with unknown disposition */
	scsi_logging_test_reset();
	tf->tscmd->cmd.result = (DID_OK << 16);
	scsi_print_result(&tf->tscmd->cmd, "Unknown disp", 0x7f);
	KUNIT_EXPECT_EQ(test, captured_count, 1);
	KUNIT_EXPECT_STREQ(
		test, captured_logs[0].msg,
		"[sda] tag#4 Unknown disp: UNKNOWN(0x7f) Result: hostbyte=DID_OK driverbyte=DRIVER_OK cmd_age=5s");
}

static struct kunit_case scsi_logging_test_cases[] = {
	KUNIT_CASE(test_sdev_prefix_printk),
	KUNIT_CASE(test_scmd_printk),
	KUNIT_CASE(test_scsi_print_command),
	KUNIT_CASE(test_scsi_print_sense_hdr),
	KUNIT_CASE(test_scsi_print_sense_buffer),
	KUNIT_CASE(test_scsi_print_sense_cmd),
	KUNIT_CASE(test_scsi_print_result),
	{}
};

static struct kunit_suite scsi_logging_test_suite = {
	.name = "scsi_logging",
	.init = scsi_logging_test_init,
	.exit = scsi_logging_test_exit,
	.test_cases = scsi_logging_test_cases,
};

kunit_test_suite(scsi_logging_test_suite);

MODULE_DESCRIPTION("SCSI logging unit tests");
MODULE_AUTHOR("Bart Van Assche");
MODULE_LICENSE("GPL");
