// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for scsi_error.c.
 *
 * Copyright (C) 2022, Oracle Corporation
 */
#include <kunit/test.h>

#include <scsi/scsi_proto.h>
#include <scsi/scsi_cmnd.h>

#define SCSI_TEST_ERROR_MAX_ALLOWED 3

static void scsi_test_error_check_passthough(struct kunit *test)
{
	struct scsi_failure multiple_sense_failures[] = {
		{
			.sense = DATA_PROTECT,
			.asc = 0x1,
			.ascq = 0x1,
			.allowed = 0,
			.result = SAM_STAT_CHECK_CONDITION,
		},
		{
			.sense = UNIT_ATTENTION,
			.asc = 0x11,
			.ascq = 0x0,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
			.result = SAM_STAT_CHECK_CONDITION,
		},
		{
			.sense = NOT_READY,
			.asc = 0x11,
			.ascq = 0x22,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
			.result = SAM_STAT_CHECK_CONDITION,
		},
		{
			.sense = ABORTED_COMMAND,
			.asc = 0x11,
			.ascq = SCMD_FAILURE_ASCQ_ANY,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
			.result = SAM_STAT_CHECK_CONDITION,
		},
		{
			.sense = HARDWARE_ERROR,
			.asc = SCMD_FAILURE_ASC_ANY,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
			.result = SAM_STAT_CHECK_CONDITION,
		},
		{
			.sense = ILLEGAL_REQUEST,
			.asc = 0x91,
			.ascq = 0x36,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
			.result = SAM_STAT_CHECK_CONDITION,
		},
		{}
	};
	struct scsi_failure retryable_host_failures[] = {
		{
			.result = DID_TRANSPORT_DISRUPTED << 16,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
		},
		{
			.result = DID_TIME_OUT << 16,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
		},
		{}
	};
	struct scsi_failure any_status_failures[] = {
		{
			.result = SCMD_FAILURE_STAT_ANY,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
		},
		{}
	};
	struct scsi_failure any_sense_failures[] = {
		{
			.result = SCMD_FAILURE_SENSE_ANY,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
		},
		{}
	};
	struct scsi_failure any_failures[] = {
		{
			.result = SCMD_FAILURE_RESULT_ANY,
			.allowed = SCSI_TEST_ERROR_MAX_ALLOWED,
		},
		{}
	};
	u8 sense[SCSI_SENSE_BUFFERSIZE] = {};
	struct scsi_cmnd sc = {
		.sense_buffer = sense,
		.failures = multiple_sense_failures,
	};
	int i;

	/* Match end of array */
	scsi_build_sense(&sc, 0, ILLEGAL_REQUEST, 0x91, 0x36);
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));
	/* Basic match in array */
	scsi_build_sense(&sc, 0, UNIT_ATTENTION, 0x11, 0x0);
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));
	/* No matching sense entry */
	scsi_build_sense(&sc, 0, MISCOMPARE, 0x11, 0x11);
	KUNIT_EXPECT_EQ(test, SCSI_RETURN_NOT_HANDLED,
			scsi_check_passthrough(&sc));
	/* Match using SCMD_FAILURE_ASCQ_ANY */
	scsi_build_sense(&sc, 0, ABORTED_COMMAND, 0x22, 0x22);
	KUNIT_EXPECT_EQ(test, SCSI_RETURN_NOT_HANDLED,
			scsi_check_passthrough(&sc));
	/* Match using SCMD_FAILURE_ASC_ANY */
	scsi_build_sense(&sc, 0, HARDWARE_ERROR, 0x11, 0x22);
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));
	/* No matching status entry */
	sc.result = SAM_STAT_RESERVATION_CONFLICT;
	KUNIT_EXPECT_EQ(test, SCSI_RETURN_NOT_HANDLED,
			scsi_check_passthrough(&sc));

	/* Test hitting allowed limit */
	scsi_build_sense(&sc, 0, NOT_READY, 0x11, 0x22);
	for (i = 0; i < SCSI_TEST_ERROR_MAX_ALLOWED; i++)
		KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));
	KUNIT_EXPECT_EQ(test, SUCCESS, scsi_check_passthrough(&sc));

	/* Match using SCMD_FAILURE_SENSE_ANY */
	sc.failures = any_sense_failures;
	scsi_build_sense(&sc, 0, MEDIUM_ERROR, 0x11, 0x22);
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));

	/* reset retries so we can retest */
	scsi_reset_failures(multiple_sense_failures);

	/* Test no retries allowed */
	sc.failures = multiple_sense_failures;
	scsi_build_sense(&sc, 0, DATA_PROTECT, 0x1, 0x1);
	KUNIT_EXPECT_EQ(test, SUCCESS, scsi_check_passthrough(&sc));

	/* No matching host byte entry */
	sc.failures = retryable_host_failures;
	sc.result = DID_NO_CONNECT << 16;
	KUNIT_EXPECT_EQ(test, SCSI_RETURN_NOT_HANDLED,
			scsi_check_passthrough(&sc));
	/* Matching host byte entry */
	sc.result = DID_TIME_OUT << 16;
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));

	/* Match SCMD_FAILURE_RESULT_ANY */
	sc.failures = any_failures;
	sc.result = DID_TRANSPORT_FAILFAST << 16;
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));

	/* Test any status handling */
	sc.failures = any_status_failures;
	sc.result = SAM_STAT_RESERVATION_CONFLICT;
	KUNIT_EXPECT_EQ(test, NEEDS_RETRY, scsi_check_passthrough(&sc));
}

static struct kunit_case scsi_test_error_cases[] = {
	KUNIT_CASE(scsi_test_error_check_passthough),
	{}
};

static struct kunit_suite scsi_test_error_suite = {
	.name = "scsi_error",
	.test_cases = scsi_test_error_cases,
};

kunit_test_suite(scsi_test_error_suite);
