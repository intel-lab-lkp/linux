// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Limited
 *
 * Author : Dev Jain <dev.jain@arm.com>
 *
 * Tests 4GB VA restriction for 32 bit process
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <linux/sizes.h>
#include <kselftest.h>

#define MAP_CHUNK_SIZE	SZ_1M
#define NR_CHUNKS_4G	(SZ_1G / MAP_CHUNK_SIZE) * 4	/* prevent overflow */

static int validate_address_hint(void)
{
	char *ptr;

	ptr = mmap((void *) (1UL << 29), MAP_CHUNK_SIZE, PROT_READ |
		   PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED)
		return 0;

	return 1;
}

int main(int argc, char *argv[])
{
	char *ptr[NR_CHUNKS_4G + 3];
	char line[1000];
	const char *file_name;
	int chunks;
	FILE *file;
	int i;

	ksft_print_header();
	ksft_set_plan(1);

	/* try allocation beyond 4 GB */
	for (i = 0; i < NR_CHUNKS_4G + 3; ++i) {
		ptr[i] = mmap(NULL, MAP_CHUNK_SIZE, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (ptr[i] == MAP_FAILED) {
			if (validate_address_hint())
				ksft_exit_fail_msg("VA exhaustion failed\n");
			break;
		}
	}

	chunks = i;
	if (chunks >= NR_CHUNKS_4G) {
		ksft_test_result_fail("mmapped chunks beyond 4GB\n");
		ksft_finished();
	}

	/* parse /proc/self/maps, confirm 32 bit VA mappings */
	file_name = "/proc/self/maps";
	file = fopen(file_name, "r");
	if (file == NULL)
		ksft_exit_fail_msg("/proc/self/maps cannot be opened\n");

	while (fgets(line, sizeof(line), file)) {
		const char *whitespace_loc, *hyphen_loc;

		hyphen_loc = strchr(line, '-');
		whitespace_loc = strchr(line, ' ');

		if (!(hyphen_loc && whitespace_loc)) {
			ksft_test_result_skip("Unexpected format");
			ksft_finished();
		}

		if ((hyphen_loc - line > 8) ||
		    (whitespace_loc - hyphen_loc) > 9) {
			ksft_test_result_fail("Memory map more than 32 bits\n");
			ksft_finished();
		}
	}

	for (int i = 0; i < chunks; ++i)
		munmap(ptr[i], MAP_CHUNK_SIZE);

	ksft_test_result_pass("Test\n");
	ksft_finished();
}
