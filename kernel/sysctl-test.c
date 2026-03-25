// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test of proc sysctl.
 */

#include <kunit/test.h>
#include <linux/sysctl.h>

#define KUNIT_PROC_READ 0
#define KUNIT_PROC_WRITE 1

/*
 * Test that proc_dointvec will not try to use a NULL .data field even when the
 * length is non-zero.
 */
static void sysctl_test_api_dointvec_null_tbl_data(struct kunit *test)
{
	/*
	 * Here we are testing that proc_dointvec behaves correctly when
	 * we give it a NULL .data field. Normally this would point to a
	 * piece of memory where the value would be stored.
	 */
	struct ctl_table null_data_table = CTLTBL_ENTRY_VNMHR(
			SYSCTL_NULL, "foo", 0644, proc_dointvec,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	/*
	 * proc_dointvec expects a buffer in user space, so we allocate one. We
	 * also need to cast it to __user so sparse doesn't get mad.
	 */
	void __user *buffer = (void __user *)kunit_kzalloc(test, sizeof(int),
							   GFP_USER);
	size_t len;
	loff_t pos;

	/*
	 * We don't care what the starting length is since proc_dointvec should
	 * not try to read because .data is NULL.
	 */
	len = 1234;
	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&null_data_table,
					       KUNIT_PROC_READ, buffer, &len,
					       &pos));
	KUNIT_EXPECT_EQ(test, 0, len);

	/*
	 * See above.
	 */
	len = 1234;
	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&null_data_table,
					       KUNIT_PROC_WRITE, buffer, &len,
					       &pos));
	KUNIT_EXPECT_EQ(test, 0, len);
}

/*
 * Similar to the previous test, we create a struct ctrl_table that has a .data
 * field that proc_dointvec cannot do anything with; however, this time it is
 * because we tell proc_dointvec that the size is 0.
 */
static void sysctl_test_api_dointvec_table_maxlen_unset(struct kunit *test)
{
	int data = 0;
	/*
	 * So .data is no longer NULL, but we tell proc_dointvec its
	 * length is 0, so it still shouldn't try to use it.
	 */
	struct ctl_table data_maxlen_unset_table = CTLTBL_ENTRY_VNMHRL(
			data, "foo", 0644, proc_dointvec,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED, 0);

	void __user *buffer = (void __user *)kunit_kzalloc(test, sizeof(int),
							   GFP_USER);
	size_t len;
	loff_t pos;

	/*
	 * As before, we don't care what buffer length is because proc_dointvec
	 * cannot do anything because its internal .data buffer has zero length.
	 */
	len = 1234;
	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&data_maxlen_unset_table,
					       KUNIT_PROC_READ, buffer, &len,
					       &pos));
	KUNIT_EXPECT_EQ(test, 0, len);

	/*
	 * See previous comment.
	 */
	len = 1234;
	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&data_maxlen_unset_table,
					       KUNIT_PROC_WRITE, buffer, &len,
					       &pos));
	KUNIT_EXPECT_EQ(test, 0, len);
}

/*
 * Here we provide a valid struct ctl_table, but we try to read and write from
 * it using a buffer of zero length, so it should still fail in a similar way as
 * before.
 */
static void sysctl_test_api_dointvec_table_len_is_zero(struct kunit *test)
{
	int data = 0;
	/* Good table. */
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo",
			0644, SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	void __user *buffer = (void __user *)kunit_kzalloc(test, sizeof(int),
							   GFP_USER);
	/*
	 * However, now our read/write buffer has zero length.
	 */
	size_t len = 0;
	loff_t pos;

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_READ, buffer,
					       &len, &pos));
	KUNIT_EXPECT_EQ(test, 0, len);

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_WRITE, buffer,
					       &len, &pos));
	KUNIT_EXPECT_EQ(test, 0, len);
}

/*
 * Test that proc_dointvec refuses to read when the file position is non-zero.
 */
static void sysctl_test_api_dointvec_table_read_but_position_set(
		struct kunit *test)
{
	int data = 0;
	/* Good table. */
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	void __user *buffer = (void __user *)kunit_kzalloc(test, sizeof(int),
							   GFP_USER);
	/*
	 * We don't care about our buffer length because we start off with a
	 * non-zero file position.
	 */
	size_t len = 1234;
	/*
	 * proc_dointvec should refuse to read into the buffer since the file
	 * pos is non-zero.
	 */
	loff_t pos = 1;

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_READ, buffer,
					       &len, &pos));
	KUNIT_EXPECT_EQ(test, 0, len);
}

/*
 * Test that we can read a two digit number in a sufficiently size buffer.
 * Nothing fancy.
 */
static void sysctl_test_dointvec_read_happy_single_positive(struct kunit *test)
{
	int data = 0;
	/* Good table. */
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	size_t len = 4;
	loff_t pos = 0;
	char *buffer = kunit_kzalloc(test, len, GFP_USER);
	char __user *user_buffer = (char __user *)buffer;
	/* Store 13 in the data field. */
	*((int *)table.data) = 13;

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_READ,
					       user_buffer, &len, &pos));
	KUNIT_ASSERT_EQ(test, 3, len);
	buffer[len] = '\0';
	/* And we read 13 back out. */
	KUNIT_EXPECT_STREQ(test, "13\n", buffer);
}

/*
 * Same as previous test, just now with negative numbers.
 */
static void sysctl_test_dointvec_read_happy_single_negative(struct kunit *test)
{
	int data = 0;
	/* Good table. */
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	size_t len = 5;
	loff_t pos = 0;
	char *buffer = kunit_kzalloc(test, len, GFP_USER);
	char __user *user_buffer = (char __user *)buffer;
	*((int *)table.data) = -16;

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_READ,
					       user_buffer, &len, &pos));
	KUNIT_ASSERT_EQ(test, 4, len);
	buffer[len] = '\0';
	KUNIT_EXPECT_STREQ(test, "-16\n", buffer);
}

/*
 * Test that a simple positive write works.
 */
static void sysctl_test_dointvec_write_happy_single_positive(struct kunit *test)
{
	int data = 0;
	/* Good table. */
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	char input[] = "9";
	size_t len = sizeof(input) - 1;
	loff_t pos = 0;
	char *buffer = kunit_kzalloc(test, len, GFP_USER);
	char __user *user_buffer = (char __user *)buffer;

	memcpy(buffer, input, len);

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_WRITE,
					       user_buffer, &len, &pos));
	KUNIT_EXPECT_EQ(test, sizeof(input) - 1, len);
	KUNIT_EXPECT_EQ(test, sizeof(input) - 1, pos);
	KUNIT_EXPECT_EQ(test, 9, *((int *)table.data));
}

/*
 * Same as previous test, but now with negative numbers.
 */
static void sysctl_test_dointvec_write_happy_single_negative(struct kunit *test)
{
	int data = 0;
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	char input[] = "-9";
	size_t len = sizeof(input) - 1;
	loff_t pos = 0;
	char *buffer = kunit_kzalloc(test, len, GFP_USER);
	char __user *user_buffer = (char __user *)buffer;

	memcpy(buffer, input, len);

	KUNIT_EXPECT_EQ(test, 0, proc_dointvec(&table, KUNIT_PROC_WRITE,
					       user_buffer, &len, &pos));
	KUNIT_EXPECT_EQ(test, sizeof(input) - 1, len);
	KUNIT_EXPECT_EQ(test, sizeof(input) - 1, pos);
	KUNIT_EXPECT_EQ(test, -9, *((int *)table.data));
}

/*
 * Test that writing a value smaller than the minimum possible value is not
 * allowed.
 */
static void sysctl_test_api_dointvec_write_single_less_int_min(
		struct kunit *test)
{
	int data = 0;
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	size_t max_len = 32, len = max_len;
	loff_t pos = 0;
	char *buffer = kunit_kzalloc(test, max_len, GFP_USER);
	char __user *user_buffer = (char __user *)buffer;
	unsigned long abs_of_less_than_min = (unsigned long)INT_MAX
					     - (INT_MAX + INT_MIN) + 1;

	/*
	 * We use this rigmarole to create a string that contains a value one
	 * less than the minimum accepted value.
	 */
	KUNIT_ASSERT_LT(test,
			(size_t)snprintf(buffer, max_len, "-%lu",
					 abs_of_less_than_min),
			max_len);

	KUNIT_EXPECT_EQ(test, -EINVAL, proc_dointvec(&table, KUNIT_PROC_WRITE,
						     user_buffer, &len, &pos));
	KUNIT_EXPECT_EQ(test, max_len, len);
	KUNIT_EXPECT_EQ(test, 0, *((int *)table.data));
}

/*
 * Test that writing the maximum possible value works.
 */
static void sysctl_test_api_dointvec_write_single_greater_int_max(
		struct kunit *test)
{
	int data = 0;
	struct ctl_table table = CTLTBL_ENTRY_VNMR(data, "foo", 0644,
			SYSCTL_ZERO, SYSCTL_ONE_HUNDRED);
	size_t max_len = 32, len = max_len;
	loff_t pos = 0;
	char *buffer = kunit_kzalloc(test, max_len, GFP_USER);
	char __user *user_buffer = (char __user *)buffer;
	unsigned long greater_than_max = (unsigned long)INT_MAX + 1;

	KUNIT_ASSERT_GT(test, greater_than_max, (unsigned long)INT_MAX);
	KUNIT_ASSERT_LT(test, (size_t)snprintf(buffer, max_len, "%lu",
					       greater_than_max),
			max_len);
	KUNIT_EXPECT_EQ(test, -EINVAL, proc_dointvec(&table, KUNIT_PROC_WRITE,
						     user_buffer, &len, &pos));
	KUNIT_ASSERT_EQ(test, max_len, len);
	KUNIT_EXPECT_EQ(test, 0, *((int *)table.data));
}

/*
 * Test each CTLTBL_ENTRY_XXX() variant.  File-scope variables are required
 * so that &var is a valid constant expression in a static initializer.
 */
static int  ctltbl_int;
static u8   ctltbl_u8;
static u8   ctltbl_u8_min, ctltbl_u8_max = 200;
static bool ctltbl_bool;
static char ctltbl_str[64];

struct ctltbl_param {
	const char       *desc;
	struct ctl_table  table;    /* built by CTLTBL_ENTRY_XXX() */
	struct ctl_table  expected; /* expected field values */
};

static const struct ctltbl_param ctltbl_cases[] = {
	/* auto handler — int */
	{
		.desc  = "sysctl_test_api_ctltbl_entry_V_int",
		.table = CTLTBL_ENTRY_V(ctltbl_int),
		.expected = { .procname = "ctltbl_int", .mode = 0444,
			      .data = &ctltbl_int, .maxlen = sizeof(int),
			      .proc_handler = proc_dointvec },
	},
	{
		.desc  = "sysctl_test_api_ctltbl_entry_VM_int",
		.table = CTLTBL_ENTRY_VM(ctltbl_int, 0644),
		.expected = { .procname = "ctltbl_int", .mode = 0644,
			      .data = &ctltbl_int, .maxlen = sizeof(int),
			      .proc_handler = proc_dointvec },
	},
	{
		.desc  = "sysctl_test_api_ctltbl_entry_VMR_int",
		.table = CTLTBL_ENTRY_VMR(ctltbl_int, 0644,
					  SYSCTL_ZERO, SYSCTL_ONE_HUNDRED),
		.expected = { .procname = "ctltbl_int", .mode = 0644,
			      .data = &ctltbl_int, .maxlen = sizeof(int),
			      .proc_handler = proc_dointvec_minmax },
	},
	{
		.desc  = "sysctl_test_api_ctltbl_entry_VN_int",
		.table = CTLTBL_ENTRY_VN(ctltbl_int, "my-sysctl"),
		.expected = { .procname = "my-sysctl", .mode = 0444,
			      .data = &ctltbl_int, .maxlen = sizeof(int),
			      .proc_handler = proc_dointvec },
	},
	{
		.desc  = "sysctl_test_api_ctltbl_entry_VNM_int",
		.table = CTLTBL_ENTRY_VNM(ctltbl_int, "my-sysctl", 0644),
		.expected = { .procname = "my-sysctl", .mode = 0644,
			      .data = &ctltbl_int, .maxlen = sizeof(int),
			      .proc_handler = proc_dointvec },
	},
	/* auto handler — u8 */
	{
		.desc  = "sysctl_test_api_ctltbl_entry_V_u8",
		.table = CTLTBL_ENTRY_V(ctltbl_u8),
		.expected = { .procname = "ctltbl_u8", .mode = 0444,
			      .data = &ctltbl_u8, .maxlen = sizeof(u8),
			      .proc_handler = proc_dou8vec_minmax },
	},
	{
		.desc  = "sysctl_test_api_ctltbl_entry_VMR_u8",
		.table = CTLTBL_ENTRY_VMR(ctltbl_u8, 0644,
					  &ctltbl_u8_min, &ctltbl_u8_max),
		.expected = { .procname = "ctltbl_u8", .mode = 0644,
			      .data = &ctltbl_u8, .maxlen = sizeof(u8),
			      .proc_handler = proc_dou8vec_minmax },
	},
	/* auto handler — bool */
	{
		.desc  = "sysctl_test_api_ctltbl_entry_V_bool",
		.table = CTLTBL_ENTRY_V(ctltbl_bool),
		.expected = { .procname = "ctltbl_bool", .mode = 0444,
			      .data = &ctltbl_bool, .maxlen = sizeof(bool),
			      .proc_handler = proc_dobool },
	},
	/* VNMH — char[] cannot be auto-dispatched by _Generic (each char[N]
	 * is a distinct type); explicit handler is required for strings.
	 * .maxlen must be sizeof(array), not sizeof(char *). */
	{
		.desc  = "sysctl_test_api_ctltbl_entry_VNMH_string",
		.table = CTLTBL_ENTRY_VNMH(ctltbl_str, "foo", 0644,
					   proc_dostring),
		.expected = { .procname = "foo", .mode = 0644,
			      .data = ctltbl_str, .maxlen = sizeof(ctltbl_str),
			      .proc_handler = proc_dostring },
	},
};

KUNIT_ARRAY_PARAM_DESC(ctltbl, ctltbl_cases, desc);

static void sysctl_test_api_ctltbl_entry(struct kunit *test)
{
	const struct ctltbl_param *p = test->param_value;

	KUNIT_EXPECT_STREQ(test, p->expected.procname, p->table.procname);
	KUNIT_EXPECT_EQ(test, p->expected.mode, p->table.mode);
	KUNIT_EXPECT_PTR_EQ(test, p->expected.data, p->table.data);
	KUNIT_EXPECT_EQ(test, p->expected.maxlen, p->table.maxlen);
	KUNIT_EXPECT_PTR_EQ(test, p->expected.proc_handler,
			p->table.proc_handler);
}

static struct kunit_case sysctl_test_cases[] = {
	KUNIT_CASE(sysctl_test_api_dointvec_null_tbl_data),
	KUNIT_CASE(sysctl_test_api_dointvec_table_maxlen_unset),
	KUNIT_CASE(sysctl_test_api_dointvec_table_len_is_zero),
	KUNIT_CASE(sysctl_test_api_dointvec_table_read_but_position_set),
	KUNIT_CASE(sysctl_test_dointvec_read_happy_single_positive),
	KUNIT_CASE(sysctl_test_dointvec_read_happy_single_negative),
	KUNIT_CASE(sysctl_test_dointvec_write_happy_single_positive),
	KUNIT_CASE(sysctl_test_dointvec_write_happy_single_negative),
	KUNIT_CASE(sysctl_test_api_dointvec_write_single_less_int_min),
	KUNIT_CASE(sysctl_test_api_dointvec_write_single_greater_int_max),
	KUNIT_CASE_PARAM(sysctl_test_api_ctltbl_entry, ctltbl_gen_params),
	{}
};

static struct kunit_suite sysctl_test_suite = {
	.name = "sysctl_test",
	.test_cases = sysctl_test_cases,
};

kunit_test_suites(&sysctl_test_suite);

MODULE_DESCRIPTION("KUnit test of proc sysctl");
MODULE_LICENSE("GPL v2");
