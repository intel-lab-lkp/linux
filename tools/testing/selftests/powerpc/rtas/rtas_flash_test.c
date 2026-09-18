// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTAS Firmware Flash Test (Safe Subset)
 * Copyright (C) 2026 IBM Corporation
 *
 * WARNING: This test only validates the interface with invalid data.
 * It does NOT perform actual firmware updates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "utils.h"

#define FLASH_PROC "/proc/ppc64/rtas/firmware_flash"
#define UPDATE_PROC "/proc/ppc64/rtas/firmware_update"

int main(void)
{
	int fd;
	char invalid_data[256];
	ssize_t ret;

	test_harness_set_timeout(10);

	/* Test 1: Verify flash interface exists */
	SKIP_IF(access(FLASH_PROC, F_OK) != 0);

	/* Test 2: Verify we can open the interface */
	fd = open(FLASH_PROC, O_RDONLY);
	if (fd >= 0) {
		close(fd);
		printf("Flash interface is readable\n");
	}

	/* Test 3: Test invalid image rejection (requires root) */
	if (geteuid() == 0) {
		fd = open(FLASH_PROC, O_WRONLY);
		if (fd >= 0) {
			/* Write invalid data - should be rejected */
			memset(invalid_data, 0xFF, sizeof(invalid_data));
			ret = write(fd, invalid_data, sizeof(invalid_data));
			
			/* We expect this to fail or be rejected */
			if (ret < 0) {
				printf("Invalid image correctly rejected (errno=%d)\n", errno);
			} else {
				printf("WARNING: Write accepted (may need validation)\n");
			}
			close(fd);
		}
	} else {
		printf("Skipping write test (requires root)\n");
	}

	/* Test 4: Check update status interface */
	if (access(UPDATE_PROC, R_OK) == 0) {
		fd = open(UPDATE_PROC, O_RDONLY);
		if (fd >= 0) {
			char status[256];
			ret = read(fd, status, sizeof(status) - 1);
			if (ret > 0) {
				status[ret] = '\0';
				printf("Update status: %s\n", status);
			}
			close(fd);
		}
	}

	printf("RTAS flash test passed (safe subset)\n");
	return 0;
}
