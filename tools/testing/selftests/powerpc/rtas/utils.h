// SPDX-License-Identifier: GPL-2.0-only
#ifndef _RTAS_TEST_UTILS_H
#define _RTAS_TEST_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Test macros */
#define SKIP_IF(x) \
	do { \
		if (x) { \
			fprintf(stderr, "SKIP: %s\n", #x); \
			exit(4); \
		} \
	} while (0)

#define FAIL_IF(x) \
	do { \
		if (x) { \
			fprintf(stderr, "FAIL: %s\n", #x); \
			exit(1); \
		} \
	} while (0)

#define FAIL_IF_MSG(x, fmt, ...) \
	do { \
		if (x) { \
			fprintf(stderr, "FAIL: " fmt "\n", ##__VA_ARGS__); \
			exit(1); \
		} \
	} while (0)

static inline void test_harness_set_timeout(int seconds)
{
	alarm(seconds);
}

#endif /* _RTAS_TEST_UTILS_H */
