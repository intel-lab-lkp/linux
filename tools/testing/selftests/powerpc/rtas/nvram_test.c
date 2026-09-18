// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVRAM Access Test
 * Copyright (C) 2026 IBM Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include "utils.h"

#define NVRAM_DEV       "/dev/nvram"
#define NVRAM_PSTORE    "/sys/fs/pstore"   /* replaces dead /proc/ppc64/nvram */
#define NVRAM_SIZE      8192               /* Typical NVRAM size */

struct nvram_partition {
	char name[64];
	unsigned int index;
};

/*
 * Discover NVRAM partitions from /sys/fs/pstore by listing entries whose
 * names contain "nvram".  On RTAS/PowerVM systems /proc/ppc64/nvram does
 * not exist; pstore is the authoritative in-kernel view of NVRAM regions.
 *
 * Returns number of partitions found, or -1 if the directory cannot be
 * opened (caller should SKIP_IF the result is < 0).
 */
static int read_nvram_partitions(struct nvram_partition *parts, int max_parts)
{
	DIR *dir;
	struct dirent *ent;
	int count = 0;

	dir = opendir(NVRAM_PSTORE);
	if (!dir)
		return -1;

	while ((ent = readdir(dir)) != NULL && count < max_parts) {
		/* Only interested in entries that contain "nvram" */
		if (strstr(ent->d_name, "nvram") == NULL)
			continue;

		/*
		 * pstore filenames follow the pattern  <type>-nvram-<id>
		 * e.g. "rtas-nvram-2", "dmesg-nvram-1".
		 * Parse the trailing numeric id as the partition index.
		 */
		strncpy(parts[count].name, ent->d_name,
			sizeof(parts[count].name) - 1);
		parts[count].name[sizeof(parts[count].name) - 1] = '\0';

		/* Extract trailing index after the last '-' */
		char *p = strrchr(ent->d_name, '-');
		parts[count].index = p ? (unsigned int)atoi(p + 1) : 0;

		count++;
	}

	closedir(dir);
	return count;
}

int main(void)
{
	int fd, ret;
	char buf[256];
	struct nvram_partition parts[32];
	int part_count;
	off_t offset;

	test_harness_set_timeout(10);

	/* Test 1: Verify /dev/nvram exists — skip gracefully if not */
	SKIP_IF(access(NVRAM_DEV, R_OK) != 0);

	/* Test 2: Open NVRAM device */
	fd = open(NVRAM_DEV, O_RDONLY);
	FAIL_IF(fd < 0);

	/*
	 * Test 3: Read NVRAM partition table from /sys/fs/pstore.
	 * Use SKIP_IF when the directory is absent (part_count < 0) so the
	 * test does not hard-fail on kernels/configs that lack pstore, while
	 * still failing when the directory exists but is empty (part_count==0).
	 */
	part_count = read_nvram_partitions(parts, 32);
	SKIP_IF(part_count < 0);
	FAIL_IF_MSG(part_count == 0, "No NVRAM partitions found in " NVRAM_PSTORE);

	printf("Found %d NVRAM partition(s) in " NVRAM_PSTORE ":\n", part_count);
	for (int i = 0; i < part_count; i++)
		printf("  %s (index=%u)\n", parts[i].name, parts[i].index);

	/* Test 4: Read from start of device */
	ret = read(fd, buf, sizeof(buf));
	FAIL_IF_MSG(ret < 0, "Failed to read from " NVRAM_DEV);

	/* Test 5: lseek to start */
	offset = lseek(fd, 0, SEEK_SET);
	FAIL_IF_MSG(offset != 0, "lseek to start failed");

	/* Test 6: lseek to arbitrary offset */
	offset = lseek(fd, 100, SEEK_SET);
	FAIL_IF_MSG(offset != 100, "lseek to offset 100 failed");

	/* Test 7: Overflow protection — read past NVRAM_SIZE must not succeed */
	/*
     	 * Test 7: Overflow protection.
     	 * Discover the real device size via SEEK_END; if the driver supports it,
     	 * attempt a read past that boundary — it must return 0 or error.
     	 * Do not use a hardcoded NVRAM_SIZE: the actual size varies by platform
     	 * and the RTAS driver will happily return data if the seek lands within
     	 * the real (larger) device.                                  FIX 2
     	 */
	off_t nvram_size = lseek(fd, 0, SEEK_END);
	if (nvram_size > 0) {
		offset = lseek(fd, nvram_size + 1000, SEEK_SET);
		ret = read(fd, buf, sizeof(buf));
		FAIL_IF_MSG(ret > 0, "Read succeeded beyond NVRAM size");
	}

	close(fd);
	printf("NVRAM test passed\n");
	return 0;
}
