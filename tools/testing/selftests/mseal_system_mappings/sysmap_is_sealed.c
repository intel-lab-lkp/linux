// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test system mapping sealing and the mseal_system_mappings= boot parameter.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <zlib.h>

#include "kselftest.h"
#include "kselftest_harness.h"

#define VMFLAGS "VmFlags:"
#define MSEAL_FLAGS "sl"
#define MAX_LINE_LEN 512
#define SYSMAP_PARAM "mseal_system_mappings="

static int parse_bool(const char *s, bool *res)
{
	if (!s)
		return -EINVAL;

	switch (tolower((unsigned char)s[0])) {
	case 'e':
	case 'y':
	case 't':
	case '1':
		*res = true;
		return 0;
	case 'd':
	case 'n':
	case 'f':
	case '0':
		*res = false;
		return 0;
	case 'o':
		switch (tolower((unsigned char)s[1])) {
		case 'n':
			*res = true;
			return 0;
		case 'f':
			*res = false;
			return 0;
		}
		break;
	}

	return -EINVAL;
}

static int sysmap_config_expected(int *enabled)
{
	struct utsname uts;
	char line[MAX_LINE_LEN];
	bool default_on = false;
	gzFile config;
	int expected;

	*enabled = -1;
	config = gzopen("/proc/config.gz", "rb");
	if (!config && !uname(&uts)) {
		snprintf(line, sizeof(line), "/boot/config-%s", uts.release);
		config = gzopen(line, "rb");
	}
	if (!config)
		return -1;

	while (gzgets(config, line, sizeof(line))) {
		if (!strcmp(line, "CONFIG_MSEAL_SYSTEM_MAPPINGS=y\n"))
			*enabled = 1;
		else if (!strcmp(line,
				 "# CONFIG_MSEAL_SYSTEM_MAPPINGS is not set\n"))
			*enabled = 0;
		else if (!strcmp(line,
				 "CONFIG_MSEAL_SYSTEM_MAPPINGS_DEFAULT_ON=y\n"))
			default_on = true;
	}
	if (!gzeof(config))
		*enabled = -1;
	expected = *enabled < 0 ? -1 : *enabled && default_on;
	gzclose(config);
	return expected;
}

static int sysmap_sealed_expected(void)
{
	char cmdline[4096], *tok, *save;
	int enabled, sealed;
	bool value;
	size_t len;
	FILE *f;

	sealed = sysmap_config_expected(&enabled);
	if (!enabled)
		return 0;

	f = fopen("/proc/cmdline", "r");
	if (!f)
		return -1;
	len = fread(cmdline, 1, sizeof(cmdline) - 1, f);
	if (ferror(f) || !feof(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	cmdline[len] = '\0';

	for (tok = strtok_r(cmdline, " \t\n", &save); tok;
	     tok = strtok_r(NULL, " \t\n", &save)) {
		if (!strcmp(tok, "--"))
			break;
		if (strncmp(tok, SYSMAP_PARAM, strlen(SYSMAP_PARAM)))
			continue;

		if (!parse_bool(tok + strlen(SYSMAP_PARAM), &value))
			sealed = value;
	}

	return sealed;
}

bool has_mapping(char *name, FILE *maps)
{
	char line[MAX_LINE_LEN];

	while (fgets(line, sizeof(line), maps)) {
		if (strstr(line, name))
			return true;
	}

	return false;
}

int mapping_is_sealed(FILE *maps)
{
	char line[MAX_LINE_LEN];

	while (fgets(line, sizeof(line), maps)) {
		if (!strncmp(line, VMFLAGS, strlen(VMFLAGS))) {
			if (strstr(line, MSEAL_FLAGS))
				return true;

			return false;
		}
	}

	return -1;
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
	bool sysmap;
};

FIXTURE_VARIANT_ADD(basic, vdso) {
	.name = "[vdso]",
	.sysmap = true,
};

FIXTURE_VARIANT_ADD(basic, vvar) {
	.name = "[vvar]",
	.sysmap = true,
};

FIXTURE_VARIANT_ADD(basic, vvar_vclock) {
	.name = "[vvar_vclock]",
	.sysmap = true,
};

FIXTURE_VARIANT_ADD(basic, sigpage) {
	.name = "[sigpage]",
	.sysmap = true,
};

FIXTURE_VARIANT_ADD(basic, vectors) {
	.name = "[vectors]",
	.sysmap = true,
};

FIXTURE_VARIANT_ADD(basic, uprobes) {
	.name = "[uprobes]",
	.sysmap = true,
};

FIXTURE_VARIANT_ADD(basic, stack) {
	.name = "[stack]",
	.sysmap = false,
};

TEST_F(basic, check_sealed)
{
	int expected = 0, sealed;

	if (!has_mapping(variant->name, self->maps)) {
		SKIP(return, "could not find the mapping, %s",
			variant->name);
	}

	sealed = mapping_is_sealed(self->maps);
	if (sealed < 0)
		SKIP(return, "could not read the mapping's sealing state");
	if (variant->sysmap) {
		expected = sysmap_sealed_expected();
		if (expected < 0)
			SKIP(return, "could not determine the expected sealing state");
	}
	EXPECT_EQ(expected, sealed);
};

static bool find_vdso(unsigned long *start, unsigned long *end)
{
	char line[MAX_LINE_LEN];
	bool found = false;
	FILE *maps;

	maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return false;

	while (fgets(line, sizeof(line), maps)) {
		if (strstr(line, "[vdso]") &&
		    sscanf(line, "%lx-%lx", start, end) == 2) {
			found = true;
			break;
		}
	}

	fclose(maps);
	return found;
}

static int vdso_is_sealed(void)
{
	int sealed = -1;
	FILE *maps;

	maps = fopen("/proc/self/smaps", "r");
	if (!maps)
		return -1;
	if (has_mapping("[vdso]", maps))
		sealed = mapping_is_sealed(maps);
	fclose(maps);
	return sealed;
}

/*
 * Unmapping or moving [vdso] must fail with EPERM when it is sealed. When it
 * is not sealed, it succeeds and leaves glibc with a stale vdso pointer, which
 * is fine because each test runs in its own forked child.
 */
TEST(vdso_munmap)
{
	int sealed;
	unsigned long start, end;
	int ret;

	if (!find_vdso(&start, &end))
		SKIP(return, "could not find the mapping, [vdso]");
	sealed = vdso_is_sealed();
	if (sealed < 0)
		SKIP(return, "could not read the [vdso] sealing state");

	ret = munmap((void *)start, end - start);
	if (sealed) {
		EXPECT_EQ(-1, ret);
		EXPECT_EQ(EPERM, errno);
	} else {
		EXPECT_EQ(0, ret);
	}
	EXPECT_EQ(sealed, find_vdso(&start, &end));
}

TEST(vdso_mremap)
{
	int sealed;
	unsigned long start, end, size, new_start;
	void *dst, *ret;

	if (!find_vdso(&start, &end))
		SKIP(return, "could not find the mapping, [vdso]");
	sealed = vdso_is_sealed();
	if (sealed < 0)
		SKIP(return, "could not read the [vdso] sealing state");
	size = end - start;

	dst = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, dst);

	ret = mremap((void *)start, size, size, MREMAP_MAYMOVE | MREMAP_FIXED,
		     dst);
	if (sealed) {
		EXPECT_EQ(MAP_FAILED, ret);
		EXPECT_EQ(EPERM, errno);
	} else {
		EXPECT_EQ(dst, ret);
	}
	ASSERT_TRUE(find_vdso(&new_start, &end));
	EXPECT_EQ(sealed ? start : (unsigned long)dst, new_start);
}

TEST_HARNESS_MAIN
