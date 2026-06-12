// SPDX-License-Identifier: GPL-2.0
/*
 * This test covers the PR_SET_NAME functionality of prctl calls
 */

#include <errno.h>
#include <sys/prctl.h>
#include <string.h>

#include "kselftest_harness.h"

#ifndef PR_SET_EXT_NAME
# define PR_SET_EXT_NAME 17
# define PR_GET_EXT_NAME 18
#endif

#define CHANGE_NAME "changename"
#define LONG_NAME	"change_to_very_long_extended_name"
#define LONG_NAME_CAP	"change_to_very_"
#define EMPTY_NAME ""
#define TASK_COMM_LEN 16
#define TASK_COMM_EXT_LEN 64
#define MAX_PATH_LEN 50

int set_name(char *name)
{
	int res;

	res = prctl(PR_SET_NAME, name, NULL, NULL, NULL);

	if (res < 0)
		return -errno;
	return res;
}

int set_ext_name(char *name)
{
	int res;

	res = prctl(PR_SET_EXT_NAME, name, NULL, NULL, NULL);

	if (res < 0)
		return -errno;
}

int check_is_name_correct(char *check_name)
{
	char name[TASK_COMM_LEN];
	int res;

	res = prctl(PR_GET_NAME, name, NULL, NULL, NULL);

	if (res < 0)
		return -errno;

	return !strcmp(name, check_name);
}

int check_is_ext_name_correct(char *check_name)
{
	char name[TASK_COMM_EXT_LEN];
	int res;

	res = prctl(PR_GET_EXT_NAME, name, NULL, NULL, NULL);

	if (res < 0)
		return -errno;

	return !strcmp(name, check_name);
}

int check_null_pointer(char *check_name)
{
	char *name = NULL;
	int res;

	res = prctl(PR_GET_NAME, name, NULL, NULL, NULL);

	return res;
}

int check_name(void)
{

	int pid;

	pid = getpid();
	FILE *fptr = NULL;
	char path[MAX_PATH_LEN] = {};
	char name[TASK_COMM_LEN] = {};
	char output[TASK_COMM_LEN] = {};
	int j;

	j = snprintf(path, MAX_PATH_LEN, "/proc/self/task/%d/comm", pid);
	fptr = fopen(path, "r");
	if (!fptr)
		return -EIO;

	fscanf(fptr, "%s", output);
	if (ferror(fptr))
		return -EIO;

	int res = prctl(PR_GET_NAME, name, NULL, NULL, NULL);

	if (res < 0)
		return -errno;

	return !strcmp(output, name);
}

TEST(rename_process) {

	EXPECT_GE(set_name(CHANGE_NAME), 0);
	EXPECT_TRUE(check_is_name_correct(CHANGE_NAME));

	EXPECT_GE(set_ext_name(LONG_NAME), 0);
	EXPECT_TRUE(check_is_ext_name_correct(LONG_NAME));
	EXPECT_TRUE(check_is_name_correct(LONG_NAME_CAP));
	EXPECT_TRUE(check_name());

	EXPECT_GE(set_name(EMPTY_NAME), 0);
	EXPECT_TRUE(check_is_name_correct(EMPTY_NAME));

	EXPECT_GE(set_name(CHANGE_NAME), 0);
	EXPECT_LT(check_null_pointer(CHANGE_NAME), 0);

	EXPECT_TRUE(check_name());
}

TEST_HARNESS_MAIN
