// SPDX-License-Identifier: GPL-2.0
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "kselftest_harness.h"

TEST(set_mm_auxv_terminator)
{
	unsigned long auxv[] = {
		AT_UID, 0x1111,
		AT_GID, 0x2222,
	};
	unsigned long result[64];
	ssize_t len;
	int fd;

	if (prctl(PR_SET_MM, PR_SET_MM_AUXV, (unsigned long)auxv,
		  sizeof(auxv), 0)) {
		if (errno == EPERM)
			SKIP(return, "requires CAP_SYS_RESOURCE");
		ASSERT_EQ(errno, 0);
	}

	fd = open("/proc/self/auxv", O_RDONLY);
	ASSERT_GE(fd, 0);

	len = read(fd, result, sizeof(result));
	close(fd);

	ASSERT_GE(len, (ssize_t)(6 * sizeof(unsigned long)));
	EXPECT_EQ(result[0], (unsigned long)AT_UID);
	EXPECT_EQ(result[1], 0x1111UL);
	EXPECT_EQ(result[2], (unsigned long)AT_GID);
	EXPECT_EQ(result[3], 0x2222UL);
	EXPECT_EQ(result[4], (unsigned long)AT_NULL);
	EXPECT_EQ(result[5], 0UL);
}

TEST_HARNESS_MAIN
