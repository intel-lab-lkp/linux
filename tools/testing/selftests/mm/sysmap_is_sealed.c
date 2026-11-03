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
#include <sys/auxv.h>
#include <sys/mman.h>

#include "kselftest.h"
#include "kselftest_harness.h"
#include "vm_util.h"

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

static bool sysmaps_sealed(void)
{
	void *vdso = (void *)getauxval(AT_SYSINFO_EHDR);

	return vdso && check_vmflag_sealed(vdso);
}

static bool sysmaps_disabled(void)
{
	char cmdline[4096];
	bool disabled = false;
	FILE *f;

	f = fopen("/proc/cmdline", "r");
	if (!f)
		return false;
	if (fgets(cmdline, sizeof(cmdline), f))
		disabled = strstr(cmdline, "no_mseal_sys_mappings");
	fclose(f);
	return disabled;
}

/*
 * Kernels built without CONFIG_MSEAL_SYSTEM_MAPPINGS don't seal system
 * mappings either, so check_sealed skips if they aren't sealed. This fails
 * instead, so run_vmtests.sh only runs it when that's set.
 */
TEST(expect_sealed)
{
	EXPECT_EQ(!sysmaps_disabled(), sysmaps_sealed());
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
	bool sealed;
};

FIXTURE_VARIANT_ADD(basic, vdso) {
	.name = "[vdso]",
	.sealed = true,
};

FIXTURE_VARIANT_ADD(basic, vvar) {
	.name = "[vvar]",
	.sealed = true,
};

FIXTURE_VARIANT_ADD(basic, vvar_vclock) {
	.name = "[vvar_vclock]",
	.sealed = true,
};

FIXTURE_VARIANT_ADD(basic, sigpage) {
	.name = "[sigpage]",
	.sealed = true,
};

FIXTURE_VARIANT_ADD(basic, vectors) {
	.name = "[vectors]",
	.sealed = true,
};

FIXTURE_VARIANT_ADD(basic, uprobes) {
	.name = "[uprobes]",
	.sealed = true,
};

FIXTURE_VARIANT_ADD(basic, stack) {
	.name = "[stack]",
	.sealed = false,
};

TEST_F(basic, check_sealed)
{
	bool disabled = sysmaps_disabled();

	if (!disabled && !sysmaps_sealed())
		SKIP(return, "system mappings are not sealed");

	if (!has_mapping(variant->name, self->maps)) {
		SKIP(return, "could not find the mapping, %s",
			variant->name);
	}

	EXPECT_EQ(variant->sealed && !disabled,
		  mapping_is_sealed(variant->name, self->maps));
};

static void *find_vdso(struct procmap_fd *procmap, size_t *size)
{
	void *vdso = (void *)getauxval(AT_SYSINFO_EHDR);

	if (!vdso || !find_vma_procmap(procmap, vdso))
		return NULL;
	*size = procmap->query.vma_end - procmap->query.vma_start;
	return (void *)(uintptr_t)procmap->query.vma_start;
}

/*
 * When [vdso] isn't sealed, these leave libc with a vDSO which it can't use,
 * which is fine since each test runs in its own child. Skip if AT_SYSINFO
 * is set, since libc may also use the vDSO for syscalls.
 */
TEST(vdso_munmap)
{
	struct procmap_fd procmap;
	bool sealed;
	size_t size;
	void *vdso;
	int ret;

	ASSERT_EQ(0, open_self_procmap(&procmap));
	vdso = find_vdso(&procmap, &size);
	if (!vdso)
		SKIP(return, "could not find the mapping, [vdso]");
	sealed = check_vmflag_sealed(vdso);
	if (!sealed && getauxval(AT_SYSINFO))
		SKIP(return, "libc makes syscalls through the vDSO");

	ret = munmap(vdso, size);
	if (sealed) {
		EXPECT_EQ(-1, ret);
		EXPECT_EQ(EPERM, errno);
	} else {
		EXPECT_EQ(0, ret);
	}
	EXPECT_EQ(sealed, find_vma_procmap(&procmap, vdso));
	close_procmap(&procmap);
}

TEST(vdso_mremap)
{
	struct procmap_fd procmap;
	void *vdso, *dst, *ret;
	bool sealed;
	size_t size;

	ASSERT_EQ(0, open_self_procmap(&procmap));
	vdso = find_vdso(&procmap, &size);
	if (!vdso)
		SKIP(return, "could not find the mapping, [vdso]");
	sealed = check_vmflag_sealed(vdso);
	if (!sealed && getauxval(AT_SYSINFO))
		SKIP(return, "libc makes syscalls through the vDSO");

	dst = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, dst);

	ret = mremap(vdso, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (sealed) {
		EXPECT_EQ(MAP_FAILED, ret);
		EXPECT_EQ(EPERM, errno);
	} else {
		EXPECT_EQ(dst, ret);
	}
	EXPECT_EQ(sealed, find_vma_procmap(&procmap, vdso));
	close_procmap(&procmap);
}

TEST(vdso_mprotect)
{
	struct procmap_fd procmap;
	bool sealed;
	size_t size;
	void *vdso;
	int ret;

	ASSERT_EQ(0, open_self_procmap(&procmap));
	vdso = find_vdso(&procmap, &size);
	if (!vdso)
		SKIP(return, "could not find the mapping, [vdso]");
	sealed = check_vmflag_sealed(vdso);
	if (!sealed && getauxval(AT_SYSINFO))
		SKIP(return, "libc makes syscalls through the vDSO");

	ret = mprotect(vdso, size, PROT_READ | PROT_WRITE);
	if (sealed) {
		EXPECT_EQ(-1, ret);
		EXPECT_EQ(EPERM, errno);
	} else {
		EXPECT_EQ(0, ret);
	}
	ASSERT_TRUE(find_vma_procmap(&procmap, vdso));
	EXPECT_EQ(sealed,
		  !(procmap.query.vma_flags & PROCMAP_QUERY_VMA_WRITABLE));
	close_procmap(&procmap);
}

TEST_HARNESS_MAIN
