// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - SysV Message Queue Scoping
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "scoped_common.h"

/*
 * Removes the message queue identified by @msqid, ignoring any error since
 * the caller might no longer have permission to operate on it (for example,
 * after entering a scoped domain).
 */
static void cleanup_msg_queue(int msqid)
{
	if (msqid >= 0)
		msgctl(msqid, IPC_RMID, NULL);
}

FIXTURE(scoped_domains)
{
	int msqid;
};

#include "scoped_base_variants.h"

FIXTURE_SETUP(scoped_domains)
{
	self->msqid = -1;
	drop_caps(_metadata);
}

/*
 * The queue is removed by the (never sandboxed) test harness parent, which
 * also runs when an assertion aborts the test after the queue got created.
 */
FIXTURE_TEARDOWN_PARENT(scoped_domains)
{
	cleanup_msg_queue(self->msqid);
}

/*
 * Parent creates a SysV message queue, then the child tries to associate
 * with it via msgget(2).  When the child is in a domain that scopes message
 * queues and the parent is not in that same scope, the association must be
 * denied with -EACCES (msgget runs the scope check via ipcperms(), which
 * masks every denial as -EACCES).
 */
TEST_F(scoped_domains, check_access_msg_queue)
{
	pid_t child;
	int status;
	int pipe_parent[2], pipe_child[2];
	char buf;
	key_t key;
	bool can_associate;

	/*
	 * The child can associate with the parent's queue unless the child
	 * is in a scoped domain that does not include the parent (i.e. the
	 * parent is outside the child's domain).
	 */
	can_associate = !variant->domain_child;

	/*
	 * Picks a per-test key derived from PID to avoid collisions.  Stale
	 * queues from a previous run are unlikely but handled by removing
	 * any matching entry before applying any scope.
	 */
	key = (key_t)(getpid() & 0x7fffffff);
	cleanup_msg_queue(msgget(key, 0));

	if (variant->domain_both)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_SYSV_MSG_QUEUE);

	ASSERT_EQ(0, pipe2(pipe_parent, O_CLOEXEC));
	ASSERT_EQ(0, pipe2(pipe_child, O_CLOEXEC));

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		int ret;

		EXPECT_EQ(0, close(pipe_child[0]));
		EXPECT_EQ(0, close(pipe_parent[1]));

		if (variant->domain_child)
			create_scoped_domain(_metadata,
					     LANDLOCK_SCOPE_SYSV_MSG_QUEUE);

		/* Signals readiness to the parent. */
		ASSERT_EQ(1, write(pipe_child[1], ".", 1));
		EXPECT_EQ(0, close(pipe_child[1]));

		/* Waits for the parent to have created the queue. */
		ASSERT_EQ(1, read(pipe_parent[0], &buf, 1));
		EXPECT_EQ(0, close(pipe_parent[0]));

		ret = msgget(key, 0);
		if (can_associate) {
			ASSERT_LE(0, ret);
		} else {
			ASSERT_EQ(-1, ret);
			/*
			 * msgget uses ipcperms(), which masks every LSM
			 * denial as -EACCES regardless of the value the
			 * LSM hook returns.
			 */
			ASSERT_EQ(EACCES, errno);
		}

		_exit(_metadata->exit_code);
		return;
	}
	EXPECT_EQ(0, close(pipe_child[1]));
	EXPECT_EQ(0, close(pipe_parent[0]));

	if (variant->domain_parent)
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_SYSV_MSG_QUEUE);

	/* Waits for the child to be ready. */
	ASSERT_EQ(1, read(pipe_child[0], &buf, 1));
	EXPECT_EQ(0, close(pipe_child[0]));

	self->msqid = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
	ASSERT_LE(0, self->msqid);

	/* Releases the child. */
	ASSERT_EQ(1, write(pipe_parent[1], ".", 1));
	EXPECT_EQ(0, close(pipe_parent[1]));

	ASSERT_EQ(child, waitpid(child, &status, 0));

	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

/*
 * The msg_queue_associate hook (exercised by msgget(2)) is covered by the
 * scoped_domains fixture above.  The remaining hooks all funnel through the
 * same scope check, so it suffices to verify that each operation is denied
 * when the child is scoped relative to the queue's creator.
 *
 * To attribute a denial to the operation under test (and not to a preceding
 * msgget(2) call), the parent creates the queue and the child inherits the
 * msqid across fork(2), bypassing msg_queue_associate.
 */
enum msg_op {
	MSG_OP_SND,
	MSG_OP_RCV,
	MSG_OP_CTL,
};

FIXTURE(scoping_msg_ops)
{
	int msqid;
};

FIXTURE_VARIANT(scoping_msg_ops)
{
	enum msg_op op;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(scoping_msg_ops, msgsnd) {
	/* clang-format on */
	.op = MSG_OP_SND,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(scoping_msg_ops, msgrcv) {
	/* clang-format on */
	.op = MSG_OP_RCV,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(scoping_msg_ops, msgctl) {
	/* clang-format on */
	.op = MSG_OP_CTL,
};

FIXTURE_SETUP(scoping_msg_ops)
{
	self->msqid = -1;
	drop_caps(_metadata);
}

/* See the scoped_domains teardown comment. */
FIXTURE_TEARDOWN_PARENT(scoping_msg_ops)
{
	cleanup_msg_queue(self->msqid);
}

TEST_F(scoping_msg_ops, deny_op)
{
	struct msgbuf {
		long mtype;
		char mtext[1];
	} msg = { .mtype = 1 };
	struct msqid_ds ds;
	pid_t child;
	int status;
	int ret = 0;

	/*
	 * The child inherits the msqid across fork(2), so no key is needed:
	 * IPC_PRIVATE always creates a new queue and cannot collide with
	 * queues left over by other processes.
	 */
	self->msqid = msgget(IPC_PRIVATE, 0600);
	ASSERT_LE(0, self->msqid);

	/* Preloads a message so msgrcv(2) would otherwise succeed. */
	ASSERT_EQ(0, msgsnd(self->msqid, &msg, sizeof(msg.mtext), 0));

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		create_scoped_domain(_metadata, LANDLOCK_SCOPE_SYSV_MSG_QUEUE);

		switch (variant->op) {
		case MSG_OP_SND:
			ret = msgsnd(self->msqid, &msg, sizeof(msg.mtext), 0);
			break;
		case MSG_OP_RCV:
			ret = msgrcv(self->msqid, &msg, sizeof(msg.mtext), 0,
				     IPC_NOWAIT);
			break;
		case MSG_OP_CTL:
			ret = msgctl(self->msqid, IPC_STAT, &ds);
			break;
		}
		ASSERT_EQ(-1, ret);
		/*
		 * msgsnd, msgrcv and msgctl(IPC_STAT) all reach the
		 * Landlock scope check via ipcperms(), whose callers map
		 * any non-zero return into -EACCES before propagating it
		 * to user space.
		 */
		ASSERT_EQ(EACCES, errno);

		_exit(_metadata->exit_code);
		return;
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));

	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

TEST_HARNESS_MAIN
