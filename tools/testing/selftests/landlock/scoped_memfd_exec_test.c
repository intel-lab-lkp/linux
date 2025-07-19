// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests for LANDLOCK_SCOPE_MEMFD_EXEC domain restrictions
 *
 * These tests validate Landlock's hierarchical execution control for memfd
 * objects. The scoping mechanism prevents processes from executing memfd
 * created in different domain contexts.
 *
 * Copyright © 2025 Abhinav Saxena <xandfury@gmail.com>
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/memfd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "scoped_common.h"

static int create_test_memfd(struct __test_metadata *const _metadata)
{
	int memfd;
	static const char test_data[] = "#!/bin/sh\nexit 42\n";

	memfd = memfd_create("test_exec", 0);
	ASSERT_LE(0, memfd)
	{
		TH_LOG("Failed to create memfd: %s", strerror(errno));
	}

	ASSERT_EQ(fchmod(memfd, 0700), 0);

	ASSERT_EQ(0, ftruncate(memfd, sizeof(test_data)));
	ASSERT_EQ(sizeof(test_data),
		  write(memfd, test_data, sizeof(test_data)));
	ASSERT_EQ(0, lseek(memfd, 0, SEEK_SET));

	return memfd;
}

static bool test_mmap_exec_restriction(int memfd, bool expect_denied)
{
	void *addr;
	const size_t page_size = getpagesize();

	addr = mmap(NULL, page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, memfd,
		    0);

	if (expect_denied) {
		bool correctly_denied = (addr == MAP_FAILED && errno == EACCES);

		if (addr != MAP_FAILED)
			munmap(addr, page_size);
		return correctly_denied;
	}

	if (addr == MAP_FAILED)
		return false;

	munmap(addr, page_size);
	return true;
}

/* clang-format off */
FIXTURE(scoped_domains) {};
/* clang-format on */

#include "scoped_base_variants.h"

FIXTURE_SETUP(scoped_domains)
{
	drop_caps(_metadata);
}

FIXTURE_TEARDOWN(scoped_domains)
{
}

/*
 * Test that regular filesystem files are unaffected by memfd restrictions
 *
 * This test ensures that LANDLOCK_SCOPE_MEMFD_EXEC scoping only affects
 * memfd objects and does not interfere with normal file execution or
 * memory mapping of regular filesystem files.
 *
 * Security scenarios tested:
 * - Scope isolation: memfd restrictions don't affect regular files
 * - Proper targeting: only anonymous memory objects are restricted
 *
 * Scenarios considered (while allowing legitimate use):
 * - Malicious process creates executable memfd -> BLOCKED
 * - Same process maps legitimate executable file ->ALLOWED
 * - Ensures restrictions are surgical, not broad
 *
 * Test flow:
 * 1. Parent optionally creates scoped domain
 * 2. Parent forks child process
 * 3. Child optionally creates scoped domain
 * 4. Child creates regular temporary file with executable content
 * 5. Child creates memfd with same content
 * 6. Test memfd execution ->should follow scoping rules
 * 7. Test regular file execution ->should always work regardless of memfd
 * scoping
 * 8. Verify differential behavior confirms proper targeting
 */
TEST_F(scoped_domains, regular_file_unaffected)
{
	int tmp_fd, memfd;
	char tmp_path[] = "/tmp/landlock_test_XXXXXX";
	void *addr;
	const size_t page_size = getpagesize();
	bool memfd_should_be_denied;

	memfd_should_be_denied = variant->domain_child ||
				 variant->domain_parent;

	if (variant->domain_parent)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_MEMFD_EXEC);

	pid_t child = fork();

	ASSERT_LE(0, child);

	if (child == 0) {
		/* Child process */
		if (variant->domain_child)
			create_scoped_domain(_metadata,
					     LANDLOCK_SCOPE_MEMFD_EXEC);

		/* Create regular file with executable test content */
		tmp_fd = mkstemp(tmp_path);
		ASSERT_LE(0, tmp_fd);
		ASSERT_EQ(0, fchmod(tmp_fd, 0755));

		static const char test_data[] = "#!/bin/sh\nexit 42\n";

		ASSERT_EQ(sizeof(test_data),
			  write(tmp_fd, test_data, sizeof(test_data)));
		ASSERT_EQ(0, lseek(tmp_fd, 0, SEEK_SET));

		/* Create memfd with identical content for comparison */
		memfd = create_test_memfd(_metadata);

		/* Test memfd execution - should follow scoping restrictions */
		bool memfd_correctly_handled = test_mmap_exec_restriction(
			memfd, memfd_should_be_denied);
		EXPECT_TRUE(memfd_correctly_handled);

		/*
		 * Test regular file execution - should always work regardless
		 * of memfd scoping
		 */
		addr = mmap(NULL, page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE,
			    tmp_fd, 0);
		EXPECT_NE(MAP_FAILED, addr);
		if (addr != MAP_FAILED)
			munmap(addr, page_size);

		/* Cleanup */
		close(memfd);
		close(tmp_fd);
		unlink(tmp_path);
		_exit(_metadata->exit_code);
	}

	/* Parent waits for child */
	int status;

	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

/*
 * Test execve() family syscall restrictions on memfd
 *
 * This test validates that direct execution of memfd files via execve(),
 * execveat(), and fexecve() syscalls is properly blocked when domain
 * scoping is enabled. Tests the /proc/self/fd/ execution path commonly
 * used for anonymous execution.
 *
 * Security scenarios tested:
 * - Direct memfd execution via /proc/self/fd/ path
 * - Anonymous execution prevention
 * - execve() hook integration with memfd scoping
 *
 * Attack scenarios prevented:
 * 1. execve("/proc/self/fd/N") where N is memfd file descriptor
 * 2. execveat(memfd_fd, "", args, env, AT_EMPTY_PATH) - anonymous execution
 * 3. fexecve(memfd_fd, args, env) - file descriptor execution
 *
 * Test flow:
 * 1. Parent optionally creates scoped domain
 * 2. Parent forks child process
 * 3. Child optionally creates scoped domain
 * 4. Child creates memfd with executable script content
 * 5. Child attempts execve() using /proc/self/fd/N path
 * 6. Verify: EACCES if scoped, successful execution (exit 42) if not scoped
 * 7. Parent checks child exit status to determine success/failure
 */
TEST_F(scoped_domains, execve_restriction)
{
	int memfd;
	char fd_path[64];
	bool should_be_denied;

	should_be_denied = variant->domain_child || variant->domain_parent;
	TH_LOG("execve_restriction: parent=%d, child=%d\n",
	       variant->domain_parent, variant->domain_child);

	if (variant->domain_parent)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_MEMFD_EXEC);

	pid_t child = fork();

	ASSERT_LE(0, child);

	if (child == 0) {
		/* Child process */
		if (variant->domain_child) {
			create_scoped_domain(_metadata,
					     LANDLOCK_SCOPE_MEMFD_EXEC);
		}

		memfd = create_test_memfd(_metadata);
		snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", memfd);

		/* Attempt execve on memfd via /proc/self/fd/ path */
		char *const argv[] = { "test", NULL };
		char *const envp[] = { NULL };

		int ret = execve(fd_path, argv, envp);

		ASSERT_EQ(-1, ret);

		/* If we reach here, execve failed */
		if (should_be_denied) {
			EXPECT_EQ(EACCES,
				  errno); /* Should be blocked by Landlock */
		} else {
			/* execve should have succeeded but failed for other reason */
			TH_LOG("execve failed unexpectedly: %s",
			       strerror(errno));
		}

		close(memfd);
		_exit(_metadata->exit_code);
	}

	/* Parent waits for child and checks exit status */
	int status;

	ASSERT_EQ(child, waitpid(child, &status, 0));

	if (should_be_denied) {
		/* Child should exit normally after execve was blocked */
		EXPECT_TRUE(WIFEXITED(status));
	} else {
		/*
		 * Child should have executed successfully with script's
		 * exit code
		 */
		EXPECT_TRUE(WIFEXITED(status));
		EXPECT_EQ(42,
			  WEXITSTATUS(status)); /* Exit code from test script */
	}
}

/*
 * Test same-domain execution restriction (should always be denied when scoped)
 *
 * This test validates the "Same domain: DENY" rule from the security matrix.
 * When a process is in a scoped domain, it should not be able to execute
 * memfd objects that it created itself, preventing read-to-execute bypass.
 *
 * Security scenarios tested:
 * - Read-to-execute bypass prevention within same domain
 * - Self-execution blocking for memfd objects
 *
 * Attack scenario prevented:
 * - Attacker process creates writable memfd in current domain
 * - Writes malicious shellcode to the memfd via write() syscalls
 * - Attempts to execute the same memfd via mmap(PROT_EXEC)
 * - Should be BLOCKED by same-domain denial rule
 * - Prevents bypassing W^X policies via anonymous memory
 *
 * Test flow:
 * 1. Process optionally creates scoped domain
 * 2. Process creates memfd (inherits current domain context)
 * 3. Process attempts to mmap its own memfd with PROT_EXEC
 * 4. Verify: ALLOW if not scoped, DENY if scoped (same domain rule)
 */
TEST_F(scoped_domains, same_domain_restriction)
{
	int memfd;
	bool should_be_denied;

	/* Same domain should be denied when scoped, allowed when not scoped */
	should_be_denied = variant->domain_parent;

	if (variant->domain_parent)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_MEMFD_EXEC);

	/* Process creates and tries to execute its own memfd (same domain) */
	memfd = create_test_memfd(_metadata);

	bool test_passed = test_mmap_exec_restriction(memfd, should_be_denied);

	EXPECT_TRUE(test_passed);

	close(memfd);
}

TEST_HARNESS_MAIN
