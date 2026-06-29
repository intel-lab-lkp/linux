// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include <sys/wait.h>

#include "kselftest_harness.h"

TEST(scm_rights_denial)
{
	int ret = system("./scm_rights_denial.sh");

	ASSERT_NE(-1, ret);
	ASSERT_TRUE(WIFEXITED(ret));

	if (WEXITSTATUS(ret) == KSFT_SKIP)
		SKIP(return, "scm_rights_denial.sh prerequisites not met");

	EXPECT_EQ(0, WEXITSTATUS(ret));
}

TEST_HARNESS_MAIN
