// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - POSIX message queue scoping
 *
 * Copyright © 2024-2026 Microsoft Corporation
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/landlock.h>
#include <mqueue.h>
#include <stddef.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "scoped_common.h"

static void set_mq_name(char *const name, const size_t size)
{
	snprintf(name, size, "/selftests-landlock-mq-tid%d", sys_gettid());
}

FIXTURE(scoped_domains)
{
	char mq_name[NAME_MAX];
};

#include "scoped_base_variants.h"

FIXTURE_SETUP(scoped_domains)
{
	drop_caps(_metadata);

	set_mq_name(self->mq_name, sizeof(self->mq_name));
	/* Removes a possibly stale queue from a previous run. */
	mq_unlink(self->mq_name);
}

FIXTURE_TEARDOWN(scoped_domains)
{
}

/*
 * The parent creates the message queue, then the child tries
 * to open it, with scoped domain(s) or no domain at all.
 */
TEST_F(scoped_domains, open_parent_queue)
{
	pid_t child;
	int status;
	int pipe_parent[2], pipe_child[2];
	char buf;
	mqd_t mq;

	ASSERT_EQ(0, pipe2(pipe_parent, O_CLOEXEC));
	ASSERT_EQ(0, pipe2(pipe_child, O_CLOEXEC));

	if (variant->domain_both)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		mqd_t mq_child;

		EXPECT_EQ(0, close(pipe_parent[1]));
		EXPECT_EQ(0, close(pipe_child[0]));

		if (variant->domain_child)
			create_scoped_domain(_metadata,
					     LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

		/* Waits for the parent to create the queue. */
		ASSERT_EQ(1, read(pipe_parent[0], &buf, 1));

		mq_child = mq_open(self->mq_name, O_RDWR);
		if (!variant->domain_child) {
			EXPECT_LE(0, mq_child);
			if (mq_child >= 0)
				EXPECT_EQ(0, mq_close(mq_child));
		} else {
			EXPECT_EQ(-1, mq_child);
			EXPECT_EQ(EPERM, errno);
		}

		ASSERT_EQ(1, write(pipe_child[1], ".", 1));
		_exit(_metadata->exit_code);
		return;
	}
	EXPECT_EQ(0, close(pipe_parent[0]));
	EXPECT_EQ(0, close(pipe_child[1]));

	if (variant->domain_parent)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

	mq = mq_open(self->mq_name, O_CREAT | O_RDWR, 0600, NULL);
	ASSERT_LE(0, mq);

	ASSERT_EQ(1, write(pipe_parent[1], ".", 1));
	ASSERT_EQ(1, read(pipe_child[0], &buf, 1));

	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_EQ(0, mq_close(mq));
	EXPECT_EQ(0, mq_unlink(self->mq_name));

	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

/*
 * The child creates the message queue, then the parent tries
 * to open it, with scoped domain(s) or no domain at all.
 */
TEST_F(scoped_domains, open_child_queue)
{
	pid_t child;
	bool can_open_child_queue;
	int status;
	int pipe_parent[2], pipe_child[2];
	char buf;
	mqd_t mq;

	can_open_child_queue = !variant->domain_parent;

	ASSERT_EQ(0, pipe2(pipe_parent, O_CLOEXEC));
	ASSERT_EQ(0, pipe2(pipe_child, O_CLOEXEC));

	if (variant->domain_both)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		mqd_t mq_child;

		EXPECT_EQ(0, close(pipe_parent[1]));
		EXPECT_EQ(0, close(pipe_child[0]));

		if (variant->domain_child)
			create_scoped_domain(_metadata,
					     LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

		/* Waits for the parent to be in a domain, if any. */
		ASSERT_EQ(1, read(pipe_parent[0], &buf, 1));

		mq_child = mq_open(self->mq_name, O_CREAT | O_RDWR, 0600, NULL);
		ASSERT_LE(0, mq_child);

		ASSERT_EQ(1, write(pipe_child[1], ".", 1));

		ASSERT_EQ(1, read(pipe_parent[0], &buf, 1));
		EXPECT_EQ(0, mq_close(mq_child));
		EXPECT_EQ(0, mq_unlink(self->mq_name));
		_exit(_metadata->exit_code);
		return;
	}
	EXPECT_EQ(0, close(pipe_parent[0]));
	EXPECT_EQ(0, close(pipe_child[1]));

	if (variant->domain_parent)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

	/* Signals that the parent is in a domain, if any. */
	ASSERT_EQ(1, write(pipe_parent[1], ".", 1));

	/* Waits for the child to create the queue. */
	ASSERT_EQ(1, read(pipe_child[0], &buf, 1));

	mq = mq_open(self->mq_name, O_RDWR);
	if (can_open_child_queue) {
		EXPECT_LE(0, mq);
		if (mq >= 0)
			EXPECT_EQ(0, mq_close(mq));
	} else {
		EXPECT_EQ(-1, mq);
		EXPECT_EQ(EPERM, errno);
	}

	/* Signals to the child that the open attempt is done. */
	ASSERT_EQ(1, write(pipe_parent[1], ".", 1));

	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

/*
 * A process must always be able to open a queue it created within its own
 * domain, whatever the enforced scope is.
 */
TEST(create_and_open_same_domain)
{
	char mq_name[NAME_MAX];
	mqd_t mq_create, mq_open_again;

	drop_caps(_metadata);
	set_mq_name(mq_name, sizeof(mq_name));
	mq_unlink(mq_name);

	create_scoped_domain(_metadata, LANDLOCK_SCOPE_POSIX_MSG_QUEUE);

	mq_create = mq_open(mq_name, O_CREAT | O_RDWR, 0600, NULL);
	ASSERT_LE(0, mq_create);

	mq_open_again = mq_open(mq_name, O_RDWR);
	EXPECT_LE(0, mq_open_again);
	if (mq_open_again >= 0)
		EXPECT_EQ(0, mq_close(mq_open_again));

	EXPECT_EQ(0, mq_close(mq_create));
	EXPECT_EQ(0, mq_unlink(mq_name));
}

TEST_HARNESS_MAIN
