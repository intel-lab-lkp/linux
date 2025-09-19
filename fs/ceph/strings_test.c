/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit tests for fs/ceph/strings.c
 *
 * Copyright (C) 2025, IBM Corporation
 *
 * Author: Viacheslav Dubeyko <Slava.Dubeyko@ibm.com>
 */

#include <kunit/test.h>
#include <linux/ceph/types.h>

#include "kunit_tests.h"

#define CEPH_KUNIT_STRINGS_TEST_RANGE		(20)
#define CEPH_KUNIT_OP_INVALID_MIN		(-999)
#define CEPH_KUNIT_OP_INVALID_MAX		(999)

typedef int (*next_op_func)(int);

struct ceph_op_iterator {
	int start;
	int cur;
	int next;
	int end;
	next_op_func get_next_op;
};

static inline
void ceph_op_iterator_init(struct ceph_op_iterator *iter,
			   int start, int end,
			   next_op_func get_next_op)
{
	if (!iter || !get_next_op)
		return;

	iter->start = iter->cur = start;
	iter->end = end;
	iter->get_next_op = get_next_op;
	iter->next = iter->get_next_op(start);
}

static inline
int ceph_op_iterator_next(struct ceph_op_iterator *iter)
{
	int threshold1, threshold2;

	if (!iter)
		return CEPH_KUNIT_OP_INVALID_MAX;

	if (iter->cur >= iter->end)
		return CEPH_KUNIT_OP_INVALID_MAX;

	threshold1 = iter->start + CEPH_KUNIT_STRINGS_TEST_RANGE;
	if (threshold1 > iter->next)
		threshold1 = iter->next;

	threshold2 = iter->next - CEPH_KUNIT_STRINGS_TEST_RANGE;
	if (threshold2 < threshold1)
		threshold2 = iter->next;

	iter->cur++;

	if (iter->cur <= threshold1)
		goto finish_method;

	if (iter->cur >= threshold2 && iter->cur <= iter->next)
		goto finish_method;

	iter->start = iter->next;
	iter->next = iter->get_next_op(iter->start);

	if (iter->start >= iter->next)
		iter->next = iter->end;

finish_method:
	return iter->cur;
}

static inline
bool ceph_op_iterator_valid(struct ceph_op_iterator *iter)
{
	if (!iter)
		return false;

	return iter->cur < iter->end;
}

static inline
int __ceph_next_op(int prev, int lower_bound, int upper_bound)
{
	if (prev < lower_bound)
		return lower_bound;

	if (prev >= lower_bound && prev < upper_bound)
		return prev + 1;

	return upper_bound;
}

#define CEPH_MDS_STATE_STR_COUNT	(CEPH_MDS_STATE_UNKNOWN_NAME_STR_IDX)
const int ceph_str_idx_2_mds_state[CEPH_MDS_STATE_STR_COUNT] = {
/* 0 */		CEPH_MDS_STATE_DNE,
/* 1 */		CEPH_MDS_STATE_STOPPED,
/* 2 */		CEPH_MDS_STATE_BOOT,
/* 3 */		CEPH_MDS_STATE_STANDBY,
/* 4 */		CEPH_MDS_STATE_STANDBY_REPLAY,
/* 5 */		CEPH_MDS_STATE_REPLAYONCE,
/* 6 */		CEPH_MDS_STATE_CREATING,
/* 7 */		CEPH_MDS_STATE_STARTING,
/* 8 */		CEPH_MDS_STATE_REPLAY,
/* 9 */		CEPH_MDS_STATE_RESOLVE,
/* 10 */	CEPH_MDS_STATE_RECONNECT,
/* 11 */	CEPH_MDS_STATE_REJOIN,
/* 12 */	CEPH_MDS_STATE_CLIENTREPLAY,
/* 13 */	CEPH_MDS_STATE_ACTIVE,
/* 14 */	CEPH_MDS_STATE_STOPPING
};

static int ceph_mds_state_next_op(int prev)
{
	if (prev < CEPH_MDS_STATE_REPLAYONCE)
		return CEPH_MDS_STATE_REPLAYONCE;

	/* [-9..-4] */
	if (prev >= CEPH_MDS_STATE_REPLAYONCE &&
	    prev < CEPH_MDS_STATE_BOOT)
		return prev + 1;

	/* [-4..-1] */
	if (prev == CEPH_MDS_STATE_BOOT)
		return CEPH_MDS_STATE_STOPPED;

	/* [-1..0] */
	if (prev >= CEPH_MDS_STATE_STOPPED &&
	    prev < CEPH_MDS_STATE_DNE)
		return prev + 1;

	/* [0..8] */
	if (prev == CEPH_MDS_STATE_DNE)
		return CEPH_MDS_STATE_REPLAY;

	/* [8..14] */
	if (prev >= CEPH_MDS_STATE_REPLAY &&
	    prev < CEPH_MDS_STATE_STOPPING)
		return prev + 1;

	return CEPH_MDS_STATE_STOPPING;
}

static void ceph_mds_state_name_test(struct kunit *test)
{
	const char *unknown_name =
		ceph_mds_state_name_strings[CEPH_MDS_STATE_UNKNOWN_NAME_STR_IDX];
	struct ceph_op_iterator iter;
	int start, end;
	int i;

	/* Test valid MDS states */
	for (i = 0; i < CEPH_MDS_STATE_STR_COUNT; i++) {
		KUNIT_EXPECT_STREQ(test,
			   ceph_mds_state_name(ceph_str_idx_2_mds_state[i]),
			   ceph_mds_state_name_strings[i]);
	}

	/* Test invalid/unknown states */
	start = CEPH_MDS_STATE_REPLAYONCE - CEPH_KUNIT_STRINGS_TEST_RANGE;
	end = CEPH_MDS_STATE_STOPPING + CEPH_KUNIT_STRINGS_TEST_RANGE;
	ceph_op_iterator_init(&iter, start, end, ceph_mds_state_next_op);

	while (ceph_op_iterator_valid(&iter)) {
		switch (ceph_mds_state_2_str_idx(iter.cur)) {
		case CEPH_MDS_STATE_UNKNOWN_NAME_STR_IDX:
			KUNIT_EXPECT_STREQ(test,
					   ceph_mds_state_name(iter.cur),
					   unknown_name);
			break;

		default:
			/* do nothing */
			break;
		}

		ceph_op_iterator_next(&iter);
	}

	KUNIT_EXPECT_STREQ(test,
			   ceph_mds_state_name(CEPH_KUNIT_OP_INVALID_MIN),
			   unknown_name);
	KUNIT_EXPECT_STREQ(test,
			   ceph_mds_state_name(CEPH_KUNIT_OP_INVALID_MAX),
			   unknown_name);
}

static int ceph_session_next_op(int prev)
{
	return __ceph_next_op(prev,
			      CEPH_SESSION_REQUEST_OPEN,
			      CEPH_SESSION_REQUEST_FLUSH_MDLOG);
}

static void ceph_session_op_name_test(struct kunit *test)
{
	const char *unknown_name =
			ceph_session_op_name_strings[CEPH_SESSION_UNKNOWN_NAME];
	struct ceph_op_iterator iter;
	int start, end;
	int i;

	/* Test valid session operations */
	for (i = CEPH_SESSION_REQUEST_OPEN; i < CEPH_SESSION_UNKNOWN_NAME; i++) {
		KUNIT_EXPECT_STREQ(test,
				   ceph_session_op_name(i),
				   ceph_session_op_name_strings[i]);
	}

	/* Test invalid/unknown operations */
	start = CEPH_SESSION_REQUEST_OPEN - CEPH_KUNIT_STRINGS_TEST_RANGE;
	end = CEPH_SESSION_REQUEST_FLUSH_MDLOG + CEPH_KUNIT_STRINGS_TEST_RANGE;
	ceph_op_iterator_init(&iter, start, end, ceph_session_next_op);

	while (ceph_op_iterator_valid(&iter)) {
		switch (ceph_session_op_2_str_idx(iter.cur)) {
		case CEPH_SESSION_UNKNOWN_NAME:
			KUNIT_EXPECT_STREQ(test,
					   ceph_session_op_name(iter.cur),
					   unknown_name);
			break;

		default:
			/* do nothing */
			break;
		}

		ceph_op_iterator_next(&iter);
	}

	KUNIT_EXPECT_STREQ(test,
			   ceph_session_op_name(CEPH_KUNIT_OP_INVALID_MIN),
			   unknown_name);
	KUNIT_EXPECT_STREQ(test,
			   ceph_session_op_name(CEPH_KUNIT_OP_INVALID_MAX),
			   unknown_name);
}

#define CEPH_MDS_OP_STR_COUNT	(CEPH_MDS_OP_UNKNOWN_NAME_STR_IDX)
const int ceph_str_idx_2_mds_op[CEPH_MDS_OP_STR_COUNT] = {
/* 0 */		CEPH_MDS_OP_LOOKUP,
/* 1 */		CEPH_MDS_OP_GETATTR,
/* 2 */		CEPH_MDS_OP_LOOKUPHASH,
/* 3 */		CEPH_MDS_OP_LOOKUPPARENT,
/* 4 */		CEPH_MDS_OP_LOOKUPINO,
/* 5 */		CEPH_MDS_OP_LOOKUPNAME,
/* 6 */		CEPH_MDS_OP_GETVXATTR,
/* 7 */		CEPH_MDS_OP_SETXATTR,
/* 8 */		CEPH_MDS_OP_RMXATTR,
/* 9 */		CEPH_MDS_OP_SETLAYOUT,
/* 10 */	CEPH_MDS_OP_SETATTR,
/* 11 */	CEPH_MDS_OP_SETFILELOCK,
/* 12 */	CEPH_MDS_OP_GETFILELOCK,
/* 13 */	CEPH_MDS_OP_SETDIRLAYOUT,
/* 14 */	CEPH_MDS_OP_MKNOD,
/* 15 */	CEPH_MDS_OP_LINK,
/* 16 */	CEPH_MDS_OP_UNLINK,
/* 17 */	CEPH_MDS_OP_RENAME,
/* 18 */	CEPH_MDS_OP_MKDIR,
/* 19 */	CEPH_MDS_OP_RMDIR,
/* 20 */	CEPH_MDS_OP_SYMLINK,
/* 21 */	CEPH_MDS_OP_CREATE,
/* 22 */	CEPH_MDS_OP_OPEN,
/* 23 */	CEPH_MDS_OP_READDIR,
/* 24 */	CEPH_MDS_OP_LOOKUPSNAP,
/* 25 */	CEPH_MDS_OP_MKSNAP,
/* 26 */	CEPH_MDS_OP_RMSNAP,
/* 27 */	CEPH_MDS_OP_LSSNAP,
/* 28 */	CEPH_MDS_OP_RENAMESNAP
};

static int ceph_mds_next_op(int prev)
{
	if (prev < CEPH_MDS_OP_LOOKUP)
		return CEPH_MDS_OP_LOOKUP;

	/* [0x00100..0x00106] */
	if (prev >= CEPH_MDS_OP_LOOKUP &&
	    prev < CEPH_MDS_OP_GETVXATTR)
		return prev + 1;

	/* [0x00106..0x00110] */
	if (prev >= CEPH_MDS_OP_GETVXATTR &&
	    prev < CEPH_MDS_OP_GETFILELOCK)
		return CEPH_MDS_OP_GETFILELOCK;

	/* [0x00110..0x00302] */
	if (prev >= CEPH_MDS_OP_GETFILELOCK &&
	    prev < CEPH_MDS_OP_OPEN)
		return CEPH_MDS_OP_OPEN;

	/* [0x00302..0x00305] */
	if (prev >= CEPH_MDS_OP_OPEN &&
	    prev < CEPH_MDS_OP_READDIR)
		return CEPH_MDS_OP_READDIR;

	/* [0x00305..0x00400] */
	if (prev >= CEPH_MDS_OP_READDIR &&
	    prev < CEPH_MDS_OP_LOOKUPSNAP)
		return CEPH_MDS_OP_LOOKUPSNAP;

	/* [0x00400..0x00402] */
	if (prev >= CEPH_MDS_OP_LOOKUPSNAP &&
	    prev < CEPH_MDS_OP_LSSNAP)
		return CEPH_MDS_OP_LSSNAP;

	/* [0x00402..0x01105] */
	if (prev >= CEPH_MDS_OP_LSSNAP &&
	    prev < CEPH_MDS_OP_SETXATTR)
		return CEPH_MDS_OP_SETXATTR;

	/* [0x01105..0x0110a] */
	if (prev >= CEPH_MDS_OP_SETXATTR &&
	    prev < CEPH_MDS_OP_SETDIRLAYOUT)
		return prev + 1;

	/* [0x0110a..0x01201] */
	if (prev >= CEPH_MDS_OP_SETDIRLAYOUT &&
	    prev < CEPH_MDS_OP_MKNOD)
		return CEPH_MDS_OP_MKNOD;

	/* [0x01201..0x01204] */
	if (prev >= CEPH_MDS_OP_MKNOD &&
	    prev < CEPH_MDS_OP_RENAME)
		return prev + 1;

	/* [0x01204..0x01220] */
	if (prev >= CEPH_MDS_OP_RENAME &&
	    prev < CEPH_MDS_OP_MKDIR)
		return CEPH_MDS_OP_MKDIR;

	/* [0x01220..0x01222] */
	if (prev >= CEPH_MDS_OP_MKDIR &&
	    prev < CEPH_MDS_OP_SYMLINK)
		return prev + 1;

	/* [0x01222..0x01301] */
	if (prev >= CEPH_MDS_OP_SYMLINK &&
	    prev < CEPH_MDS_OP_CREATE)
		return CEPH_MDS_OP_CREATE;

	/* [0x01301..0x01400] */
	if (prev >= CEPH_MDS_OP_CREATE &&
	    prev < CEPH_MDS_OP_MKSNAP)
		return CEPH_MDS_OP_MKSNAP;

	/* [0x01400..0x01401] */
	if (prev >= CEPH_MDS_OP_MKSNAP &&
	    prev < CEPH_MDS_OP_RMSNAP)
		return prev + 1;

	/* [0x01401..0x01403] */
	if (prev >= CEPH_MDS_OP_RMSNAP &&
	    prev < CEPH_MDS_OP_RENAMESNAP)
		return CEPH_MDS_OP_RENAMESNAP;

	return CEPH_MDS_OP_RENAMESNAP;
}

static void ceph_mds_op_name_test(struct kunit *test)
{
	const char *unknown_name =
		ceph_mds_op_name_strings[CEPH_MDS_OP_UNKNOWN_NAME_STR_IDX];
	struct ceph_op_iterator iter;
	int start, end;
	int i;

	/* Test valid MDS operations */
	for (i = 0; i < CEPH_MDS_OP_STR_COUNT; i++) {
		KUNIT_EXPECT_STREQ(test,
				   ceph_mds_op_name(ceph_str_idx_2_mds_op[i]),
				   ceph_mds_op_name_strings[i]);
	}

	/* Test invalid/unknown operations */
	start = CEPH_MDS_OP_LOOKUP - CEPH_KUNIT_STRINGS_TEST_RANGE;
	end = CEPH_MDS_OP_RENAMESNAP + CEPH_KUNIT_STRINGS_TEST_RANGE;
	ceph_op_iterator_init(&iter, start, end, ceph_mds_next_op);

	while (ceph_op_iterator_valid(&iter)) {
		switch (ceph_mds_op_2_str_idx(iter.cur)) {
		case CEPH_MDS_OP_UNKNOWN_NAME_STR_IDX:
			KUNIT_EXPECT_STREQ(test,
					   ceph_mds_op_name(iter.cur),
					   unknown_name);
			break;

		default:
			/* do nothing */
			break;
		}

		ceph_op_iterator_next(&iter);
	}

	KUNIT_EXPECT_STREQ(test, ceph_mds_op_name(-0x99999), unknown_name);
	KUNIT_EXPECT_STREQ(test, ceph_mds_op_name(0x99999), unknown_name);
}

static int ceph_cap_next_op(int prev)
{
	return __ceph_next_op(prev,
			      CEPH_CAP_OP_GRANT,
			      CEPH_CAP_OP_RENEW);
}

static void ceph_cap_op_name_test(struct kunit *test)
{
	const char *unknown_name =
			ceph_cap_op_name_strings[CEPH_CAP_OP_UNKNOWN_NAME];
	struct ceph_op_iterator iter;
	int start, end;
	int i;

	/* Test valid capability operations */
	for (i = 0; i < CEPH_CAP_OP_UNKNOWN_NAME; i++) {
		KUNIT_EXPECT_STREQ(test,
				   ceph_cap_op_name(i),
				   ceph_cap_op_name_strings[i]);
	}

	/* Test invalid/unknown operations */
	start = CEPH_CAP_OP_GRANT - CEPH_KUNIT_STRINGS_TEST_RANGE;
	end = CEPH_CAP_OP_RENEW + CEPH_KUNIT_STRINGS_TEST_RANGE;
	ceph_op_iterator_init(&iter, start, end, ceph_cap_next_op);

	while (ceph_op_iterator_valid(&iter)) {
		switch (ceph_cap_op_2_str_idx(iter.cur)) {
		case CEPH_CAP_OP_UNKNOWN_NAME:
			KUNIT_EXPECT_STREQ(test,
					   ceph_cap_op_name(iter.cur),
					   unknown_name);
			break;

		default:
			/* do nothing */
			break;
		}

		ceph_op_iterator_next(&iter);
	}

	KUNIT_EXPECT_STREQ(test,
			   ceph_cap_op_name(CEPH_KUNIT_OP_INVALID_MIN),
			   unknown_name);
	KUNIT_EXPECT_STREQ(test,
			   ceph_cap_op_name(CEPH_KUNIT_OP_INVALID_MAX),
			   unknown_name);
}

#define CEPH_MDS_LEASE_OP_STR_COUNT	(CEPH_MDS_LEASE_UNKNOWN_NAME_STR_IDX)
const int ceph_str_idx_2_lease_op[CEPH_MDS_LEASE_OP_STR_COUNT] = {
/* 0 */		CEPH_MDS_LEASE_REVOKE,
/* 1 */		CEPH_MDS_LEASE_RELEASE,
/* 2 */		CEPH_MDS_LEASE_RENEW,
/* 3 */		CEPH_MDS_LEASE_REVOKE_ACK
};

static int ceph_lease_next_op(int prev)
{
	return __ceph_next_op(prev,
			      CEPH_MDS_LEASE_REVOKE,
			      CEPH_MDS_LEASE_REVOKE_ACK);
}

static void ceph_lease_op_name_test(struct kunit *test)
{
	const char *unknown_name =
		ceph_lease_op_name_strings[CEPH_MDS_LEASE_UNKNOWN_NAME_STR_IDX];
	struct ceph_op_iterator iter;
	int start, end;
	int i;

	/* Test valid lease operations */
	for (i = 0; i < CEPH_MDS_LEASE_OP_STR_COUNT; i++) {
		KUNIT_EXPECT_STREQ(test,
				   ceph_lease_op_name(ceph_str_idx_2_lease_op[i]),
				   ceph_lease_op_name_strings[i]);
	}

	/* Test invalid/unknown operations */
	start = CEPH_MDS_LEASE_REVOKE - CEPH_KUNIT_STRINGS_TEST_RANGE;
	end = CEPH_MDS_LEASE_REVOKE_ACK + CEPH_KUNIT_STRINGS_TEST_RANGE;
	ceph_op_iterator_init(&iter, start, end, ceph_lease_next_op);

	while (ceph_op_iterator_valid(&iter)) {
		switch (ceph_lease_op_2_str_idx(iter.cur)) {
		case CEPH_MDS_LEASE_UNKNOWN_NAME_STR_IDX:
			KUNIT_EXPECT_STREQ(test,
					   ceph_lease_op_name(iter.cur),
					   unknown_name);
			break;

		default:
			/* do nothing */
			break;
		}

		ceph_op_iterator_next(&iter);
	}

	KUNIT_EXPECT_STREQ(test,
			   ceph_lease_op_name(CEPH_KUNIT_OP_INVALID_MIN),
			   unknown_name);
	KUNIT_EXPECT_STREQ(test,
			   ceph_lease_op_name(CEPH_KUNIT_OP_INVALID_MAX),
			   unknown_name);
}

static int ceph_snap_next_op(int prev)
{
	return __ceph_next_op(prev,
			      CEPH_SNAP_OP_UPDATE,
			      CEPH_SNAP_OP_SPLIT);
}

static void ceph_snap_op_name_test(struct kunit *test)
{
	const char *unknown_name =
		ceph_snap_op_name_strings[CEPH_SNAP_OP_UNKNOWN_NAME];
	struct ceph_op_iterator iter;
	int start, end;
	int i;

	/* Test valid snapshot operations */
	for (i = 0; i < CEPH_SNAP_OP_UNKNOWN_NAME; i++) {
		KUNIT_EXPECT_STREQ(test,
				   ceph_snap_op_name(i),
				   ceph_snap_op_name_strings[i]);
	}

	/* Test invalid/unknown operations */
	start = CEPH_SNAP_OP_UPDATE - CEPH_KUNIT_STRINGS_TEST_RANGE;
	end = CEPH_SNAP_OP_SPLIT + CEPH_KUNIT_STRINGS_TEST_RANGE;
	ceph_op_iterator_init(&iter, start, end, ceph_snap_next_op);

	while (ceph_op_iterator_valid(&iter)) {
		switch (ceph_snap_op_2_str_idx(iter.cur)) {
		case CEPH_MDS_LEASE_UNKNOWN_NAME_STR_IDX:
			KUNIT_EXPECT_STREQ(test,
					   ceph_snap_op_name(iter.cur),
					   unknown_name);
			break;

		default:
			/* do nothing */
			break;
		}

		ceph_op_iterator_next(&iter);
	}

	KUNIT_EXPECT_STREQ(test,
			   ceph_snap_op_name(CEPH_KUNIT_OP_INVALID_MIN),
			   unknown_name);
	KUNIT_EXPECT_STREQ(test,
			   ceph_snap_op_name(CEPH_KUNIT_OP_INVALID_MAX),
			   unknown_name);
}

static struct kunit_case ceph_strings_test_cases[] = {
	KUNIT_CASE(ceph_mds_state_name_test),
	KUNIT_CASE(ceph_session_op_name_test),
	KUNIT_CASE(ceph_mds_op_name_test),
	KUNIT_CASE(ceph_cap_op_name_test),
	KUNIT_CASE(ceph_lease_op_name_test),
	KUNIT_CASE(ceph_snap_op_name_test),
	{}
};

static struct kunit_suite ceph_strings_test_suite = {
	.name = "ceph_strings",
	.test_cases = ceph_strings_test_cases,
};

kunit_test_suites(&ceph_strings_test_suite);

MODULE_AUTHOR("Viacheslav Dubeyko <slava@dubeyko.com>");
MODULE_DESCRIPTION("KUnit tests for ceph strings functions");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
