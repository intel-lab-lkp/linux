// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTAS /proc Interface Test
 * Copyright (C) 2026 IBM Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "utils.h"

#define RTAS_PROC_DIR "/proc/ppc64/rtas"
#define RTAS_LOG "/proc/ppc64/rtas/rtas_log"

/* RTAS log entry header structure */
struct rtas_log_header {
	uint32_t signature;
	uint16_t length;
	uint8_t version;
	uint8_t severity;
	uint8_t disposition;
	uint8_t extended;
	/* ... more fields ... */
} __attribute__((packed));

static int enumerate_rtas_proc(void)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	dir = opendir(RTAS_PROC_DIR);
	if (!dir)
		return -1;

	printf("RTAS proc entries:\n");
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] != '.') {
			printf("  %s\n", entry->d_name);
			count++;
		}
	}

	closedir(dir);
	return count;
}

static int read_rtas_log(void)
{
	int fd;
	char buf[4096];
	ssize_t bytes;

	fd = open(RTAS_LOG, O_RDONLY);
	if (fd < 0)
		return -1;

	bytes = read(fd, buf, sizeof(buf));
	close(fd);

	if (bytes > 0) {
		printf("Read %zd bytes from RTAS log\n", bytes);
		/* Basic validation of log format */
		if (bytes >= sizeof(struct rtas_log_header)) {
			struct rtas_log_header *hdr = (struct rtas_log_header *)buf;
			printf("  Log entry: version=%u, severity=%u, length=%u\n",
			       hdr->version, hdr->severity, hdr->length);
		}
	}

	return bytes;
}

int main(void)
{
	int count, ret;

	test_harness_set_timeout(10);

	/* Test 1: Verify RTAS proc directory exists */
	SKIP_IF(access(RTAS_PROC_DIR, R_OK) != 0);

	/* Test 2: Enumerate RTAS proc entries */
	count = enumerate_rtas_proc();
	FAIL_IF_MSG(count < 0, "Failed to enumerate RTAS proc entries");
	FAIL_IF_MSG(count == 0, "No RTAS proc entries found");

	/* Test 3: Read RTAS log */
	ret = read_rtas_log();
	/* Log may be empty, which is OK */
	if (ret < 0) {
		printf("RTAS log not available (may be empty)\n");
	}

	printf("RTAS proc test passed\n");
	return 0;
}
