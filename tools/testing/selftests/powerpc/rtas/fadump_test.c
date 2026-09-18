// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware-Assisted Dump (FADump) Test
 * Copyright (C) 2026 IBM Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "utils.h"

#define FADUMP_DIR "/sys/kernel/fadump"
#define FADUMP_ENABLED "/sys/kernel/fadump/enabled"
#define FADUMP_REGISTERED "/sys/kernel/fadump/registered"
#define FADUMP_MEM_RESERVED "/sys/kernel/fadump/mem_reserved"
#define FADUMP_REGISTER "/sys/kernel/fadump/fadump_register"

static int read_sysfs_int(const char *path)
{
	FILE *fp;
	int value = -1;

	fp = fopen(path, "r");
	if (fp) {
		fscanf(fp, "%d", &value);
		fclose(fp);
	}
	return value;
}

static unsigned long read_sysfs_ulong(const char *path)
{
	FILE *fp;
	unsigned long value = 0;

	fp = fopen(path, "r");
	if (fp) {
		fscanf(fp, "%lu", &value);
		fclose(fp);
	}
	return value;
}

int main(void)
{
	int enabled, registered;
	unsigned long mem_reserved;

	test_harness_set_timeout(10);

	/* Test 1: Verify FADump sysfs directory exists */
	SKIP_IF(access(FADUMP_DIR, R_OK) != 0);

	/* Test 2: Read enabled status */
	enabled = read_sysfs_int(FADUMP_ENABLED);
	FAIL_IF_MSG(enabled < 0, "Failed to read FADump enabled status");
	printf("FADump enabled: %d\n", enabled);

	/* Test 3: Read registered status */
	registered = read_sysfs_int(FADUMP_REGISTERED);
	FAIL_IF_MSG(registered < 0, "Failed to read FADump registered status");
	printf("FADump registered: %d\n", registered);

	/* Test 4: Read reserved memory */
	mem_reserved = read_sysfs_ulong(FADUMP_MEM_RESERVED);
	printf("FADump reserved memory: %lu bytes\n", mem_reserved);

	if (enabled) {
		FAIL_IF_MSG(mem_reserved == 0, 
		            "FADump enabled but no memory reserved");
	}

	/* Test 5: Verify sysfs attributes are readable */
	FAIL_IF(access(FADUMP_ENABLED, R_OK) != 0);
	FAIL_IF(access(FADUMP_REGISTERED, R_OK) != 0);
	FAIL_IF(access(FADUMP_MEM_RESERVED, R_OK) != 0);

	/* Note: We don't test actual registration/unregistration here
	 * as it requires root and could affect system stability */

	printf("FADump test passed\n");
	return 0;
}
