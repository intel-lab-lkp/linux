// SPDX-License-Identifier: GPL-2.0-only
#ifndef __SELFTESTS_X86_COREDUMP_H
#define __SELFTESTS_X86_COREDUMP_H

#include <link.h>
#include <stdint.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "kselftest.h"

#define CORE_PATTERN_PATH "/proc/sys/kernel/core_pattern"
#ifndef CORENAME_MAX_SIZE
#define CORENAME_MAX_SIZE 128
#endif

static char orig_pattern[CORENAME_MAX_SIZE];

static inline bool get_core_pattern(char *buf, int size)
{
	bool ret;
	FILE *fp;

	if (!buf)
		return false;

	fp = fopen(CORE_PATTERN_PATH, "r");
	if (!fp)
		return false;

	ret = fgets(buf, size, fp) != NULL;
	if (ret)
		buf[strcspn(buf, "\n")] = 0;

	fclose(fp);

	return ret;
}

static inline bool set_core_pattern(const char *pattern)
{
	bool ret;
	FILE *fp;

	if (!pattern)
		return false;

	fp = fopen(CORE_PATTERN_PATH, "w");
	if (!fp)
		return false;

	ret = fprintf(fp, "%s\n", pattern) >= 0;

	fclose(fp);

	return ret;
}

static inline bool save_core_pattern(void)
{
	return get_core_pattern(orig_pattern, CORENAME_MAX_SIZE);
}

static inline bool set_test_core_pattern(void)
{
	const char test_pattern[] = "core.%p";

	if (!strcmp(orig_pattern, test_pattern))
		return true;

	return set_core_pattern(test_pattern);
}

static inline bool restore_core_pattern(void)
{
	return set_core_pattern(orig_pattern);
}

static char core_dir[] = "/tmp/x86-coredump-XXXXXX";

static struct corefile {
	bool dumped;
	int fd;
	struct stat st;
	char *image;
	char path[PATH_MAX];
} core_file = { false, -1, { }, NULL, "\0" };

static inline void corefile_reset(void)
{
	close(core_file.fd);
	unlink(core_file.path);

	if (core_file.image != NULL && core_file.image != MAP_FAILED)
		munmap((void *)core_file.image, core_file.st.st_size);

	core_file = (struct corefile){ .dumped = false, .fd = -1, .st = { },
				       .image = NULL, .path = "\0" };
}

static inline void coredump_cleanup(void)
{
	if (core_file.dumped)
		corefile_reset();

	rmdir(core_dir);
	strcpy(core_dir, "/tmp/x86-coredump-XXXXXX");
	restore_core_pattern();
}

/*
 * Create a temporary directory for the core file and configure core_pattern to
 * use a predictable file name. Save the original pattern so it can be restored
 * after the test.
 */
static inline bool coredump_prepare(void)
{
	struct rlimit lim = { RLIM_INFINITY, RLIM_INFINITY };

	if (setrlimit(RLIMIT_CORE, &lim)) {
		ksft_test_result_skip("Failed to set rlimit. core Try sudo.\n");
		return false;
	}

	if (!save_core_pattern()) {
		ksft_test_result_skip("Failed to save core pattern.\n");
		return false;
	}

	if (!set_test_core_pattern()) {
		ksft_test_result_skip("Failed to update core pattern. Try sudo.\n");
		return false;
	}

	if (!mkdtemp(core_dir)) {
		coredump_cleanup();
		ksft_test_result_skip("Failed to create test folder.\n");
		return false;
	}

	return true;
}

/* Always call this for any failed exit after coredump_prepare() */
static inline void coredump_exit_fail_msg(const char *msg)
{
	coredump_cleanup();
	ksft_exit_fail_msg("%s\n", msg);
}

static inline void coredump_trigger(void)
{
	volatile unsigned int *ptr = NULL;

	*ptr = 0xDEAD;
}

static inline void corefile_map(pid_t child)
{
	snprintf(core_file.path, sizeof(core_file.path), "%s/core.%d", core_dir, child);

	core_file.fd = open(core_file.path, O_RDONLY);
	if (core_file.fd < 0)
		coredump_exit_fail_msg("Cannot open core file.");

	core_file.dumped = true;

	if (fstat(core_file.fd, &core_file.st))
		coredump_exit_fail_msg("Invalid core file.");

	core_file.image = mmap(NULL, core_file.st.st_size, PROT_READ, MAP_PRIVATE, core_file.fd, 0);
	if (core_file.image == MAP_FAILED)
		coredump_exit_fail_msg("Cannot map core file.");
}

#ifndef NT_X86_XSTATE
#define NT_X86_XSTATE	0x202
#endif

#define next_pos(pos, size)		((pos) + (((size) + 3) & ~3))

/* Program Header */
#define phdr_ptr(core, offset)		(ElfW(Phdr) *)(core + offset)

/* Note Header */
#define nhdr_ptr(core, offset)		(ElfW(Nhdr) *)(core + offset)

/*
 * Locate the NT_X86_XSTATE note in the ELF core image and return a pointer to
 * its XSTATE data.
 */
static inline char *retrieve_corefile_xbuf(void)
{
	ElfW(Ehdr) *ehdr;
	ElfW(Phdr) *phdr;
	ElfW(Nhdr) *nhdr;
	size_t pos, end;
	int i;

	ehdr = (ElfW(Ehdr) *)core_file.image;
	phdr = phdr_ptr(core_file.image, ehdr->e_phoff);

	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type != PT_NOTE)
			continue;

		end = phdr[i].p_offset + phdr[i].p_filesz;
		pos = phdr[i].p_offset;

		while (pos + sizeof(*nhdr) <= end) {
			nhdr  = nhdr_ptr(core_file.image, pos);
			pos   = next_pos(pos + sizeof(*nhdr), nhdr->n_namesz);

			if (nhdr->n_type == NT_X86_XSTATE)
				return core_file.image + pos;

			pos = next_pos(pos, nhdr->n_descsz);
		}
	}

	coredump_exit_fail_msg("No NT_X86_XSTATE was found.");
	_exit(1);
}

#endif /* __SELFTESTS_X86_COREDUMP_H */
