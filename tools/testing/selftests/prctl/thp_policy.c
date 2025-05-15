// SPDX-License-Identifier: GPL-2.0
/*
 * This test covers the PR_GET/SET_THP_POLICY functionality of prctl calls
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#ifndef PR_SET_THP_POLICY
#define PR_SET_THP_POLICY		78
#define PR_GET_THP_POLICY		79
#define PR_THP_POLICY_DEFAULT_HUGE	0
#define PR_THP_POLICY_DEFAULT_NOHUGE	1
#define PR_THP_POLICY_SYSTEM		2
#endif

#define CONTENT_SIZE 256
#define BUF_SIZE (12 * 2 * 1024 * 1024) // 12 x 2MB pages

enum system_policy {
	SYSTEM_POLICY_ALWAYS,
	SYSTEM_POLICY_MADVISE,
	SYSTEM_POLICY_NEVER,
};

int system_thp_policy;

/* check if the sysfs file contains the expected substring */
static int check_file_content(const char *file_path, const char *expected_substring)
{
	FILE *file = fopen(file_path, "r");
	char buffer[CONTENT_SIZE];

	if (!file) {
		perror("Failed to open file");
		return -1;
	}
	if (fgets(buffer, CONTENT_SIZE, file) == NULL) {
		perror("Failed to read file");
		fclose(file);
		return -1;
	}
	fclose(file);
	// Remove newline character from the buffer
	buffer[strcspn(buffer, "\n")] = '\0';
	if (strstr(buffer, expected_substring))
		return 0;
	else
		return 1;
}

/*
 * The test is designed for 2M hugepages only.
 * Check if hugepage size is 2M, if 2M size inherits from global
 * setting, and if the global setting is madvise or always.
 */
static int sysfs_check(void)
{
	int res = 0;

	res = check_file_content("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "2097152");
	if (res) {
		printf("hpage_pmd_size is not set to 2MB. Skipping test.\n");
		return -1;
	}
	res |= check_file_content("/sys/kernel/mm/transparent_hugepage/hugepages-2048kB/enabled",
				  "[inherit]");
	if (res) {
		printf("hugepages-2048kB does not inherit global setting. Skipping test.\n");
		return -1;
	}

	res = check_file_content("/sys/kernel/mm/transparent_hugepage/enabled", "[madvise]");
	if (!res) {
		system_thp_policy = SYSTEM_POLICY_MADVISE;
		return 0;
	}
	res = check_file_content("/sys/kernel/mm/transparent_hugepage/enabled", "[always]");
	if (!res) {
		system_thp_policy = SYSTEM_POLICY_ALWAYS;
		return 0;
	}
	printf("Global THP policy not set to madvise or always. Skipping test.\n");
	return -1;
}

static int check_smaps_for_huge(void)
{
	FILE *file = fopen("/proc/self/smaps", "r");
	int is_anonhuge = 0;
	char line[256];

	if (!file) {
		perror("fopen");
		return -1;
	}

	while (fgets(line, sizeof(line), file)) {
		if (strstr(line, "AnonHugePages:") && strstr(line, "24576 kB")) {
			is_anonhuge = 1;
			break;
		}
	}
	fclose(file);
	return is_anonhuge;
}

static int test_mmap_thp(int madvise_buffer)
{
	int is_anonhuge;

	char *buffer = (char *)mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buffer == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	if (madvise_buffer)
		madvise(buffer, BUF_SIZE, MADV_HUGEPAGE);

	// set memory to ensure it's allocated
	memset(buffer, 0, BUF_SIZE);
	is_anonhuge = check_smaps_for_huge();
	munmap(buffer, BUF_SIZE);
	return is_anonhuge;
}

/* Global policy is always, process is changed to NOHUGE (process becomes madvise) */
static int test_global_always_process_nohuge(void)
{
	int is_anonhuge = 0, res = 0, status = 0;
	pid_t pid;

	if (prctl(PR_SET_THP_POLICY, PR_THP_POLICY_DEFAULT_NOHUGE, NULL, NULL, NULL) != 0) {
		perror("prctl failed to set policy to madvise");
		return -1;
	}

	/* Make sure prctl changes are carried across fork */
	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	res = prctl(PR_GET_THP_POLICY, NULL, NULL, NULL, NULL);
	if (res != PR_THP_POLICY_DEFAULT_NOHUGE) {
		printf("prctl PR_GET_THP_POLICY returned %d pid %d\n", res, pid);
		goto err_out;
	}

	/* global = always, process = madvise, we shouldn't get HPs without madvise */
	is_anonhuge = test_mmap_thp(0);
	if (is_anonhuge) {
		printf(
		"PR_THP_POLICY_DEFAULT_NOHUGE set but still got hugepages without MADV_HUGEPAGE\n");
		goto err_out;
	}

	is_anonhuge = test_mmap_thp(1);
	if (!is_anonhuge) {
		printf(
		"PR_THP_POLICY_DEFAULT_NOHUGE set but did't get hugepages with MADV_HUGEPAGE\n");
		goto err_out;
	}

	/* Reset to system policy */
	if (prctl(PR_SET_THP_POLICY, PR_THP_POLICY_SYSTEM, NULL, NULL, NULL) != 0) {
		perror("prctl failed to set policy to system");
		goto err_out;
	}

	is_anonhuge = test_mmap_thp(0);
	if (!is_anonhuge) {
		printf("global policy is always but we still didn't get hugepages\n");
		goto err_out;
	}

	is_anonhuge = test_mmap_thp(1);
	if (!is_anonhuge) {
		printf("global policy is always but we still didn't get hugepages\n");
		goto err_out;
	}

	if (pid == 0) {
		exit(EXIT_SUCCESS);
	} else {
		wait(&status);
		if (WIFEXITED(status))
			return 0;
		else
			return -1;
	}

err_out:
	if (pid == 0)
		exit(EXIT_FAILURE);
	else
		return -1;
}

/* Global policy is madvise, process is changed to HUGE (process becomes always) */
static int test_global_madvise_process_huge(void)
{
	int is_anonhuge = 0, res = 0, status = 0;
	pid_t pid;

	if (prctl(PR_SET_THP_POLICY, PR_THP_POLICY_DEFAULT_HUGE, NULL, NULL, NULL) != 0) {
		perror("prctl failed to set process policy to always");
		return -1;
	}

	/* Make sure prctl changes are carried across fork */
	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	res = prctl(PR_GET_THP_POLICY, NULL, NULL, NULL, NULL);
	if (res != PR_THP_POLICY_DEFAULT_HUGE) {
		printf("prctl PR_GET_THP_POLICY returned %d pid %d\n", res, pid);
		goto err_out;
	}

	/* global = madvise, process = always, we should get HPs irrespective of MADV_HUGEPAGE */
	is_anonhuge = test_mmap_thp(0);
	if (!is_anonhuge) {
		printf("PR_THP_POLICY_DEFAULT_HUGE set but didn't get hugepages\n");
		goto err_out;
	}

	is_anonhuge = test_mmap_thp(1);
	if (!is_anonhuge) {
		printf("PR_THP_POLICY_DEFAULT_HUGE set but did't get hugepages\n");
		goto err_out;
	}

	/* Reset to system policy */
	if (prctl(PR_SET_THP_POLICY, PR_THP_POLICY_SYSTEM, NULL, NULL, NULL) != 0) {
		perror("prctl failed to set policy to system");
		goto err_out;
	}

	is_anonhuge = test_mmap_thp(0);
	if (is_anonhuge) {
		printf("global policy is madvise\n");
		goto err_out;
	}

	is_anonhuge = test_mmap_thp(1);
	if (!is_anonhuge) {
		printf("global policy is madvise\n");
		goto err_out;
	}

	if (pid == 0) {
		exit(EXIT_SUCCESS);
	} else {
		wait(&status);
		if (WIFEXITED(status))
			return 0;
		else
			return -1;
	}
err_out:
	if (pid == 0)
		exit(EXIT_FAILURE);
	else
		return -1;
}

int main(void)
{
	if (sysfs_check())
		return 0;

	if (system_thp_policy == SYSTEM_POLICY_ALWAYS)
		return test_global_always_process_nohuge();
	else
		return test_global_madvise_process_huge();
}
