// SPDX-License-Identifier: GPL-2.0-only
/*
 * test system mappings are sealed when
 * KCONFIG_MSEAL_SYSTEM_MAPPINGS=y
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#define VDSO_NAME "[vdso]"
#define VVAR_NAME "[vvar]"
#define VVAR_VCLOCK_NAME "[vvar_vclock]"
#define UPROBES_NAME "[uprobes]"
#define SIGPAGE_NAME "[sigpage]"
#define VECTORS_NAME "[vectors]"

#define VMFLAGS "VmFlags:"
#define MSEAL_FLAGS "sl"
#define MAX_LINE_LEN 512

bool has_mapping(char *name, FILE *maps)
{
	char line[MAX_LINE_LEN];

	while (fgets(line, sizeof(line), maps)) {
		if (strstr(line, name))
			return true;
	}

	return false;
}

bool mapping_is_sealed(char *name, FILE *maps)
{
	char line[MAX_LINE_LEN];

	while (fgets(line, sizeof(line), maps)) {
		if (!strncmp(line, VMFLAGS, strlen(VMFLAGS))) {
			if (strstr(line, MSEAL_FLAGS))
				return true;

			return false;
		}
	}

	return false;
}

FIXTURE(basic) {
	FILE *maps;
};

FIXTURE_SETUP(basic)
{
	self->maps = fopen("/proc/self/smaps", "r");
	if (!self->maps)
		SKIP(return, "Could not open /proc/self/smap, errno=%d",
			errno);
};

FIXTURE_TEARDOWN(basic)
{
	if (self->maps)
		fclose(self->maps);
};

FIXTURE_VARIANT(basic)
{
	char *name;
};

FIXTURE_VARIANT_ADD(basic, vdso) {
	.name = VDSO_NAME,
};

FIXTURE_VARIANT_ADD(basic, vvar) {
	.name = VVAR_NAME,
};

FIXTURE_VARIANT_ADD(basic, vvar_vclock) {
	.name = VVAR_VCLOCK_NAME,
};

FIXTURE_VARIANT_ADD(basic, sigpage) {
	.name = SIGPAGE_NAME,
};

FIXTURE_VARIANT_ADD(basic, vectors) {
	.name = VECTORS_NAME,
};

FIXTURE_VARIANT_ADD(basic, uprobes) {
	.name = UPROBES_NAME,
};

TEST_F(basic, is_sealed)
{
	if (!has_mapping(variant->name, self->maps)) {
		SKIP(return, "could not found the mapping, %s",
			variant->name);
	}

	EXPECT_TRUE(mapping_is_sealed(variant->name, self->maps));
};

TEST_HARNESS_MAIN
