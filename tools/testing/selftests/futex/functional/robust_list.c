// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Igalia S.L.
 *
 * Robust list test by André Almeida <andrealmeid@igalia.com>
 *
 * The robust list uAPI allows userspace to create "robust" locks, in the sense
 * that if the lock holder thread dies, the remaining threads that are waiting
 * for the lock won't block forever, waiting for a lock that will never be
 * released.
 *
 * This is achieve by userspace setting a list where a thread can enter all the
 * locks (futexes) that it is holding. The robust list is a linked list, and
 * userspace register the start of the list with the syscall set_robust_list().
 * If such thread eventually dies, the kernel will walk this list, waking up one
 * thread waiting for each futex and marking the futex word with the flag
 * FUTEX_OWNER_DIED.
 *
 * See also
 *	man set_robust_list
 *	Documententation/locking/robust-futex-ABI.rst
 *	Documententation/locking/robust-futexes.rst
 */

#define _GNU_SOURCE

#include "futextest.h"
#include "kselftest_harness.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define STACK_SIZE (1024 * 1024)
#define FUTEX_TIMEOUT 3
#define SLEEP_US 100

#if __SIZEOF_LONG__ == 8
# define BUILD_64
#endif

#ifndef SYS_set_robust_list2
# define SYS_set_robust_list2 473

enum robust_list_cmd {
	FUTEX_ROBUST_LIST_CMD_CREATE_64,
	FUTEX_ROBUST_LIST_CMD_CREATE_32,
	FUTEX_ROBUST_LIST_CMD_MODIFY_64,
	FUTEX_ROBUST_LIST_CMD_MODIFY_32,
	FUTEX_ROBUST_LIST_CMD_LIST_LIMIT,
	FUTEX_ROBUST_LIST_CMD_USER_MAX,

	/*
	 * Kernel internal, rejected for user space
	 */
	FUTEX_ROBUST_LIST_SET_NATIVE = 128,
	FUTEX_ROBUST_LIST_SET_COMPAT,
};

struct robust_list32 {
	uint32_t next;
};

struct robust_list_head32 {
	struct robust_list32	list;
	int32_t			futex_offset;
	uint32_t		list_op_pending;
};
#endif

static pthread_barrier_t barrier, barrier2;

static int set_robust_list(struct robust_list_head *head, size_t len)
{
	return syscall(SYS_set_robust_list, head, len);
}

static int get_robust_list(int pid, struct robust_list_head **head, size_t *len_ptr)
{
	return syscall(SYS_get_robust_list, pid, head, len_ptr);
}

static int sys_futex_robust_unlock(_Atomic(uint32_t) *uaddr, unsigned int op, int val,
				   void *list_op_pending, unsigned int val3)
{
	return syscall(SYS_futex, uaddr, op, val, NULL, list_op_pending, val3, 0);
}

static int set_robust_list2(struct robust_list_head *head, enum robust_list_cmd cmd,
			    unsigned int index, unsigned int flags)
{
	return syscall(SYS_set_robust_list2, head, cmd, index, flags, 0, 0);
}

static bool robust_list2_support(void)
{
	int ret = set_robust_list2(NULL, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0, 0);

	if (ret == -1 && errno == ENOSYS)
		return false;

	return true;
}

/*
 * Return the set command according to the app bitness
 */
static int get_cmd_create(void)
{
	return sizeof(uintptr_t) == 8 ? FUTEX_ROBUST_LIST_CMD_CREATE_64 :
	       FUTEX_ROBUST_LIST_CMD_CREATE_64;
}

static int get_cmd_modify(void)
{
	return sizeof(uintptr_t) == 8 ? FUTEX_ROBUST_LIST_CMD_MODIFY_64 :
	       FUTEX_ROBUST_LIST_CMD_MODIFY_64;
}

FIXTURE(robust_api) {};

FIXTURE_VARIANT(robust_api)
{
	bool robust2;
};

FIXTURE_SETUP(robust_api)
{
	if (!variant->robust2)
		return;

	if (!robust_list2_support())
		SKIP(return, "robust_list2 not supported");
}

FIXTURE_TEARDOWN(robust_api) {}

FIXTURE_VARIANT_ADD(robust_api, robust1)
{
	.robust2 = false,
};

FIXTURE_VARIANT_ADD(robust_api, robust2)
{
	.robust2 = true,
};

/*
 * Basic lock struct, contains just the futex word and the robust list element
 * Real implementations have also a *prev to easily walk in the list
 */
typedef _Atomic(unsigned int) atomic_futex_t;

struct lock_struct {
	atomic_futex_t		futex;
	struct robust_list	list;
	bool			robust2;
};

struct lock_struct32 {
	_Atomic(uint32_t)	futex;
	struct robust_list32	list;
};

struct child_args {
	struct __test_metadata	*_metadata;
	void			*arg;
};

/*
 * Helper function to spawn a child thread. Returns -1 on error, pid on success
 */
static int create_child(struct __test_metadata *_metadata, int (*fn)(void *arg), void *arg)
{
	struct child_args *cargs = malloc(sizeof(*cargs));
	char *stack;
	pid_t pid;

	if (!cargs)
		return -1;
	cargs->_metadata = _metadata;
	cargs->arg = arg;

	stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		free(cargs);
		return -1;
	}

	stack += STACK_SIZE;

	pid = clone(fn, stack, CLONE_VM | SIGCHLD, cargs);
	if (pid == -1) {
		free(cargs);
		return -1;
	}

	return pid;
}

/*
 * Helper function to prepare and register a robust list
 */
static int set_list(struct robust_list_head *head, bool robust2, int *index)
{
	int ret;

	head->futex_offset = (size_t) offsetof(struct lock_struct, futex) -
			     (size_t) offsetof(struct lock_struct, list);
	head->list.next = &head->list;
	head->list_op_pending = NULL;

	if (!robust2)
		return set_robust_list(head, sizeof(*head));

	ret = set_robust_list2(head, get_cmd_create(), 0, 0);

	if (ret >= 0 && index)
		*index = ret;

	return ret;
}

/*
 * Change a given list index to a different head
 */
static int modify_list(struct robust_list_head *head, int index)
{
	return set_robust_list2(head, get_cmd_modify(), index, 0);
}

/*
 * A basic (and incomplete) mutex lock function with robustness
 */
static int mutex_lock(struct lock_struct *lock, struct robust_list_head *head, bool error_inject)
{
	atomic_futex_t *futex = &lock->futex;
	unsigned int zero = 0;
	pid_t tid = gettid();
	int ret = -1;

	/*
	 * Set list_op_pending before starting the lock, so the kernel can catch
	 * the case where the thread died during the lock operation
	 */
	head->list_op_pending = &lock->list;

	if (atomic_compare_exchange_strong(futex, &zero, tid)) {
		/*
		 * We took the lock, insert it in the robust list
		 */
		struct robust_list *list = &head->list;

		/* Error injection to test list_op_pending */
		if (error_inject)
			return 0;

		while (list->next != &head->list)
			list = list->next;

		list->next = &lock->list;
		lock->list.next = &head->list;

		ret = 0;
	} else {
		/*
		 * We didn't take the lock, wait until the owner wakes (or dies)
		 */
		struct timespec to;

		to.tv_sec = FUTEX_TIMEOUT;
		to.tv_nsec = 0;

		tid = atomic_load(futex);
		/* Kernel ignores futexes without the waiters flag */
		tid |= FUTEX_WAITERS;
		atomic_store(futex, tid);

		ret = futex_wait((futex_t *) futex, tid, &to, 0);

		/*
		 * A real mutex_lock() implementation would loop here to finally
		 * take the lock. We don't care about that, so we stop here.
		 */
	}

	head->list_op_pending = NULL;

	return ret;
}

/*
 * This child thread will succeed taking the lock, and then will exit holding it
 */
static int child_fn_lock(void *arg)
{
	struct child_args *cargs = arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct lock_struct *lock = cargs->arg;
	struct robust_list_head head;
	int ret, index;

	free(cargs);

	ret = set_list(&head, lock->robust2, &index);
	ASSERT_NE(ret, -1)
		TH_LOG("set_robust_list error");

	ret = mutex_lock(lock, &head, false);
	ASSERT_EQ(ret, 0)
		TH_LOG("mutex_lock error");

	pthread_barrier_wait(&barrier);

	/*
	 * There's a race here: the parent thread needs to be inside
	 * futex_wait() before the child thread dies, otherwise it will miss the
	 * wakeup from handle_futex_death() that this child will emit. We wait a
	 * little bit just to make sure that this happens.
	 */
	usleep(SLEEP_US);

	return 0;
}

/*
 * Spawns a child thread that will set a robust list, take the lock, register it
 * in the robust list and die. The parent thread will wait on this futex, and
 * should be waken up when the child exits.
 */
TEST_F(robust_api, test_robustness)
{
	struct lock_struct lock = { .futex = 0 };
	atomic_futex_t *futex = &lock.futex;
	struct robust_list_head head;
	int ret, pid, wstatus, index;

	lock.robust2 = variant->robust2;

	ret = set_list(&head, lock.robust2, &index);
	ASSERT_NE(ret, -1);

	/*
	 * Lets use a barrier to ensure that the child thread takes the lock
	 * before the parent
	 */
	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);

	pid = create_child(_metadata, &child_fn_lock, &lock);
	ASSERT_NE(pid, -1);

	pthread_barrier_wait(&barrier);
	ret = mutex_lock(&lock, &head, false);

	/*
	 * futex_wait() should return 0 and the futex word should be marked with
	 * FUTEX_OWNER_DIED
	 */
	ASSERT_EQ(ret, 0);

	ASSERT_TRUE(*futex & FUTEX_OWNER_DIED);

	wait(&wstatus);
	pthread_barrier_destroy(&barrier);

	EXPECT_EQ(WEXITSTATUS(wstatus), 0)
		TH_LOG("child failed");
}

/*
 * The only valid value for len is sizeof(*head)
 */
TEST(test_set_robust_list_invalid_size)
{
	struct robust_list_head head;
	size_t head_size = sizeof(head);
	int ret;

	ret = set_robust_list(&head, head_size);
	ASSERT_EQ(ret, 0);

	ret = set_robust_list(&head, head_size * 2);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);

	ret = set_robust_list(&head, head_size - 1);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);

	ret = set_robust_list(&head, 0);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);
}

/*
 * Test invalid parameters
 */
TEST(test_set_robust_list2_inval)
{
	struct robust_list_head head;
	int ret;

	if (!robust_list2_support())
		SKIP(return, "robust_list2 not supported\n");

	/* Bad flag */
	ret = set_robust_list2(&head, get_cmd_create(), 0, 999);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);
}

/*
 * Test get_robust_list with pid = 0, getting the list of the running thread
 */
TEST(test_get_robust_list_self)
{
	struct robust_list_head head, head2, *get_head;
	size_t head_size = sizeof(head), len_ptr;
	int ret;

	ret = set_robust_list(&head, head_size);
	ASSERT_EQ(ret, 0);

	ret = get_robust_list(0, &get_head, &len_ptr);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(get_head, &head);
	ASSERT_EQ(head_size, len_ptr);

	ret = set_robust_list(&head2, head_size);
	ASSERT_EQ(ret, 0);

	ret = get_robust_list(0, &get_head, &len_ptr);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(get_head, &head2);
	ASSERT_EQ(head_size, len_ptr);
}

static int child_list(void *arg)
{
	struct child_args *cargs = arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct robust_list_head *head = cargs->arg;
	int ret;

	free(cargs);

	ret = set_robust_list(head, sizeof(*head));
	ASSERT_EQ(ret, 0)
		TH_LOG("set_robust_list error");

	/*
	 * After setting the list head, wait until the main thread can call
	 * get_robust_list() for this thread before exiting.
	 */
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier2);

	return 0;
}

/*
 * Test get_robust_list from another thread. We use two barriers here to ensure
 * that:
 *   1) the child thread set the list before we try to get it from the
 * parent
 *   2) the child thread still alive when we try to get the list from it
 */
TEST(test_get_robust_list_child)
{
	struct robust_list_head head, *get_head;
	int ret, wstatus;
	size_t len_ptr;
	pid_t tid;

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ret = pthread_barrier_init(&barrier2, NULL, 2);
	ASSERT_EQ(ret, 0);

	tid = create_child(_metadata, &child_list, &head);
	ASSERT_NE(tid, -1);

	pthread_barrier_wait(&barrier);

	ret = get_robust_list(tid, &get_head, &len_ptr);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(&head, get_head);

	pthread_barrier_wait(&barrier2);

	wait(&wstatus);
	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	EXPECT_EQ(WEXITSTATUS(wstatus), 0)
		TH_LOG("child failed");
}

static int child_fn_lock_with_error(void *arg)
{
	struct child_args *cargs = arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct lock_struct *lock = cargs->arg;
	struct robust_list_head head;
	int ret, index;

	free(cargs);

	ret = set_list(&head, lock->robust2, &index);
	ASSERT_NE(ret, -1)
		TH_LOG("set_robust_list error");

	ret = mutex_lock(lock, &head, true);
	ASSERT_EQ(ret, 0)
		TH_LOG("mutex_lock error");

	pthread_barrier_wait(&barrier);

	/* See comment at child_fn_lock() */
	usleep(SLEEP_US);

	return 0;
}

/*
 * Same as robustness test, but inject an error where the mutex_lock() exits
 * earlier, just after setting list_op_pending and taking the lock, to test the
 * list_op_pending mechanism
 */
TEST_F(robust_api, test_set_list_op_pending)
{
	struct lock_struct lock = { .futex = 0 };
	atomic_futex_t *futex = &lock.futex;
	struct robust_list_head head;
	int ret, wstatus, index;

	lock.robust2 = variant->robust2;

	ret = set_list(&head, lock.robust2, &index);
	ASSERT_NE(ret, -1);

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);

	ret = create_child(_metadata, &child_fn_lock_with_error, &lock);
	ASSERT_NE(ret, -1);

	pthread_barrier_wait(&barrier);
	ret = mutex_lock(&lock, &head, false);

	ASSERT_EQ(ret, 0);

	ASSERT_TRUE(*futex & FUTEX_OWNER_DIED);

	wait(&wstatus);
	pthread_barrier_destroy(&barrier);

	EXPECT_EQ(WEXITSTATUS(wstatus), 0)
		TH_LOG("child failed");
}

#define CHILD_NR 10

static int child_lock_holder(void *arg)
{
	struct child_args *cargs = arg;
	struct lock_struct *locks = cargs->arg;
	struct robust_list_head head;
	int i, index;

	free(cargs);

	set_list(&head, locks[0].robust2, &index);

	for (i = 0; i < CHILD_NR; i++) {
		locks[i].futex = 0;
		mutex_lock(&locks[i], &head, false);
	}

	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier2);

	/* See comment at child_fn_lock() */
	usleep(SLEEP_US);

	return 0;
}

static int child_wait_lock(void *arg)
{
	struct child_args *cargs = arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct lock_struct *lock = cargs->arg;
	struct robust_list_head head;
	int ret;

	free(cargs);

	pthread_barrier_wait(&barrier2);
	ret = mutex_lock(lock, &head, false);
	ASSERT_EQ(ret, 0)
		TH_LOG("mutex_lock error");

	ASSERT_TRUE(lock->futex & FUTEX_OWNER_DIED)
		TH_LOG("futex not marked with FUTEX_OWNER_DIED");

	return 0;
}

/*
 * Test a robust list of more than one element. All the waiters should wake when
 * the holder dies
 */
TEST_F(robust_api, test_robust_list_multiple_elements)
{
	struct lock_struct locks[CHILD_NR];
	pid_t pids[CHILD_NR + 1];
	int i, ret, wstatus;

	if (variant->robust2 && !robust_list2_support())
		SKIP(return, "robust_list2 not supported\n");

	locks[0].robust2 = variant->robust2;

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);
	ret = pthread_barrier_init(&barrier2, NULL, CHILD_NR + 1);
	ASSERT_EQ(ret, 0);


	pids[0] = create_child(_metadata, &child_lock_holder, &locks);
	ASSERT_NE(pids[0], -1);

	/* Wait until the locker thread takes the look */
	pthread_barrier_wait(&barrier);

	for (i = 0; i < CHILD_NR; i++) {
		pids[i+1] = create_child(_metadata, &child_wait_lock, &locks[i]);
		ASSERT_NE(pids[i+1], -1);
	}

	/* Wait for all children to return (holder + all waiters) */
	ret = 0;
	for (i = 0; i < CHILD_NR + 1; i++) {
		waitpid(pids[i], &wstatus, 0);
		if (WEXITSTATUS(wstatus))
			ret = -1;
	}

	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	EXPECT_EQ(ret, 0)
		TH_LOG("One or more children failed");
}

static int child_lock_holder_multiple_lists(void *arg)
{
	struct child_args *cargs = arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct lock_struct *locks = cargs->arg;
	struct robust_list_head *heads;
	int i, list_limit, index, ret;

	list_limit = set_robust_list2(NULL, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0, 0);
	ASSERT_GT(list_limit, 1);

	heads = malloc(list_limit * sizeof(*heads));
	ASSERT_TRUE((uintptr_t) heads);

	/*
	 * Try to clear any exiting list, ignore errors if they didn't exit
	 */
	for (i = 0; i < list_limit; i++)
		modify_list(NULL, i);

	/*
	 * Given that this thread has no robust list attached yet, it should
	 * get as return all available lists in order [0, list_limit)
	 */
	for (i = 0; i < list_limit; i++) {
		ret = set_list(&heads[i], true, &index);
		ASSERT_EQ(ret, i);
		locks[i].futex = 0;
		mutex_lock(&locks[i], &heads[i], false);
	}

	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier2);

	/* See comment at child_fn_lock() */
	usleep(SLEEP_US * 10);

	return 0;
}

/*
 * Similar to test_robust_list_multiple_elements, but instead of one list with
 * several elements, create several lists with one element.
 */
TEST(test_robust_list_multiple_lists)
{
	int i, ret, wstatus, list_limit;
	struct lock_struct *locks;
	pid_t *pids;

	if (!robust_list2_support())
		SKIP(return, "robust_list2 not supported\n");

	list_limit = set_robust_list2(NULL, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0, 0);
	ASSERT_GT(list_limit, 1);

	locks = malloc(list_limit * sizeof(*locks));
	ASSERT_NE(locks, NULL);

	pids = malloc(list_limit * sizeof(*pids));
	ASSERT_NE(pids, NULL);

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);
	ret = pthread_barrier_init(&barrier2, NULL, list_limit + 1);
	ASSERT_EQ(ret, 0);

	pids[0] = create_child(_metadata, &child_lock_holder_multiple_lists, locks);

	/* Wait until the locker thread takes the look */
	pthread_barrier_wait(&barrier);

	for (i = 0; i < list_limit; i++)
		pids[i+1] = create_child(_metadata, &child_wait_lock, &locks[i]);

	/* Wait for all children to return */
	ret = 0;

	for (i = 0; i < list_limit; i++) {
		waitpid(pids[i], &wstatus, 0);
		if (WEXITSTATUS(wstatus))
			ret = -1;
	}

	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	free(locks);
	free(pids);
}

static int child_circular_list(void *arg)
{
	struct child_args *cargs = arg;
	bool robust2 = cargs->arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	static struct lock_struct a, b, c;
	struct robust_list_head head;
	int ret, index;

	free(cargs);

	ret = set_list(&head, robust2, &index);
	ASSERT_NE(ret, -1)
		TH_LOG("set_list error");

	head.list.next = &a.list;

	/*
	 * The last element should point to head list, but we short circuit it
	 */
	a.list.next = &b.list;
	b.list.next = &c.list;
	c.list.next = &a.list;

	return 0;
}

/*
 * Create a circular robust list. The kernel should be able to destroy the list
 * while processing it so it won't be trapped in an infinite loop while handling
 * a process exit
 */
TEST_F(robust_api, test_circular_list)
{
	bool robust2 = variant->robust2;
	int wstatus;
	pid_t pid;

	create_child(_metadata, child_circular_list, (void *) robust2);
	pid = create_child(_metadata, child_circular_list, NULL);
	ASSERT_NE(pid, -1);

	wait(&wstatus);

	EXPECT_EQ(WEXITSTATUS(wstatus), 0)
		TH_LOG("child failed");
}

/*
 * Below are tests for the fix of robust release race condition. Please read the following
 * thread to learn more about the issue in the first place and why the following functions fix it:
 * https://lore.kernel.org/lkml/20260316162316.356674433@kernel.org/
 */

/*
 * Auxiliary code for binding the vDSO functions
 */
static void *get_vdso_func_addr(const char *function)
{
	const char *vdso_names[] = {
		"linux-vdso.so.1", "linux-gate.so.1", "linux-vdso32.so.1", "linux-vdso64.so.1",
	};

	for (int i = 0; i < ARRAY_SIZE(vdso_names); i++) {
		void *vdso = dlopen(vdso_names[i], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);

		if (vdso)
			return dlsym(vdso, function);
	}
	return NULL;
}

/*
 * These are the real vDSO function signatures:
 *
 *	__vdso_futex_robust_list64_try_unlock(__u32 *lock, __u32 tid, __u64 *pop)
 *	__vdso_futex_robust_list32_try_unlock(__u32 *lock, __u32 tid, __u32 *pop)
 *
 * So for the generic entry point we need to use a void pointer as the last argument
 */
FIXTURE(vdso_unlock)
{
	uint32_t (*vdso)(_Atomic(uint32_t) *lock, uint32_t tid, void *pop);
};

FIXTURE_VARIANT(vdso_unlock)
{
	bool is_32;
	char func_name[];
};

FIXTURE_SETUP(vdso_unlock)
{
	self->vdso = get_vdso_func_addr(variant->func_name);
}

FIXTURE_TEARDOWN(vdso_unlock) {}

FIXTURE_VARIANT_ADD(vdso_unlock, 32)
{
	.func_name = "__vdso_futex_robust_list32_try_unlock",
	.is_32 = true,
};

FIXTURE_VARIANT_ADD(vdso_unlock, 64)
{
	.func_name = "__vdso_futex_robust_list64_try_unlock",
	.is_32 = false,
};

/*
 * Test the vDSO robust_listXX_try_unlock() for the uncontended case. The virtual syscall should
 * return the thread ID of the lock owner, the lock word must be 0 and the list_op_pending should
 * be NULL.
 */
TEST_F(vdso_unlock, test_robust_try_unlock_uncontended)
{
	struct lock_struct lock = { .futex = 0 };
	_Atomic(unsigned int) *futex = &lock.futex;
	struct robust_list_head head;
	uintptr_t exp = (uintptr_t) NULL;
	pid_t tid = gettid();
	int ret;

	if (!self->vdso) {
		ksft_test_result_skip("%s not found\n", variant->func_name);
		return;
	}

	*futex = tid;

	ret = set_list(&head, false, NULL);
	if (ret == -1)
		ksft_test_result_fail("set_robust_list error\n");

	head.list_op_pending = &lock.list;

	ret = self->vdso(futex, tid, &head.list_op_pending);

	ASSERT_EQ(ret, tid);
	ASSERT_EQ(*futex, 0);

	/* Check only the lower 32 bits for the 32-bit entry point */
	if (variant->is_32) {
		exp = (uintptr_t)(unsigned long)&lock.list;
		exp &= ~0xFFFFFFFFULL;
	}

	ASSERT_EQ((uintptr_t)(unsigned long)head.list_op_pending, exp);
}

/*
 * If the lock is contended, the operation fails. The return value is the value found at the
 * futex word (tid | FUTEX_WAITERS), the futex word is not modified and the list_op_pending is_32
 * not cleared.
 */
TEST_F(vdso_unlock, test_robust_try_unlock_contended)
{
	struct lock_struct lock = { .futex = 0 };
	_Atomic(unsigned int) *futex = &lock.futex;
	struct robust_list_head head;
	pid_t tid = gettid();
	int ret;

	if (!self->vdso) {
		ksft_test_result_skip("%s not found\n", variant->func_name);
		return;
	}

	*futex = tid | FUTEX_WAITERS;

	ret = set_list(&head, false, NULL);
	if (ret == -1)
		ksft_test_result_fail("set_robust_list error\n");

	head.list_op_pending = &lock.list;

	ret = self->vdso(futex, tid, &head.list_op_pending);

	ASSERT_EQ(ret, tid | FUTEX_WAITERS);
	ASSERT_EQ(*futex, tid | FUTEX_WAITERS);
	ASSERT_EQ(head.list_op_pending, &lock.list);
}

FIXTURE(futex_op) {};

FIXTURE_VARIANT(futex_op)
{
	unsigned int op;
	unsigned int val3;
};

FIXTURE_SETUP(futex_op) {}

FIXTURE_TEARDOWN(futex_op) {}

FIXTURE_VARIANT_ADD(futex_op, wake)
{
	.op = FUTEX_WAKE,
	.val3 = 0,
};

FIXTURE_VARIANT_ADD(futex_op, wake_bitset)
{
	.op = FUTEX_WAKE_BITSET,
	.val3 = FUTEX_BITSET_MATCH_ANY,
};

FIXTURE_VARIANT_ADD(futex_op, unlock_pi)
{
	.op = FUTEX_UNLOCK_PI,
	.val3 = 0,
};

FIXTURE_VARIANT_ADD(futex_op, wake32)
{
	.op = FUTEX_WAKE | FUTEX_ROBUST_LIST32,
	.val3 = 0,
};

FIXTURE_VARIANT_ADD(futex_op, wake_bitset32)
{
	.op = FUTEX_WAKE_BITSET | FUTEX_ROBUST_LIST32,
	.val3 = FUTEX_BITSET_MATCH_ANY,
};

FIXTURE_VARIANT_ADD(futex_op, unlock_pi32)
{
	.op = FUTEX_UNLOCK_PI | FUTEX_ROBUST_LIST32,
	.val3 = 0,
};

/*
 * The syscall should return the number of tasks waken (for this test, 0), clear the futex word and
 * clear list_op_pending
 */
TEST_F(futex_op, test_futex_robust_unlock)
{
	struct lock_struct lock = { .futex = 0 };
	_Atomic(unsigned int) *futex = &lock.futex;
	uintptr_t exp = (uintptr_t) NULL;
	struct robust_list_head head;
	pid_t tid = gettid();
	int ret;

#ifndef BUILD_64
	if (!(variant->op & FUTEX_ROBUST_LIST32)) {
		ksft_test_result_skip("Not supported for 32 bit build\n");
		return;
	}
#endif

	*futex = tid | FUTEX_WAITERS;

	ret = set_list(&head, false, 0);
	if (ret == -1)
		ksft_test_result_fail("set_robust_list error\n");

	head.list_op_pending = &lock.list;

	ret = sys_futex_robust_unlock(futex, FUTEX_ROBUST_UNLOCK | variant->op, tid,
				      &head.list_op_pending, variant->val3);

	if (ret == -1 && errno == ENOSYS)
		SKIP(return, "No support for FUTEX_ROBUST_UNLOCK");

	ASSERT_EQ(ret, 0);
	ASSERT_EQ(*futex, 0);

	if (variant->op & FUTEX_ROBUST_LIST32) {
		exp = (uint64_t)(unsigned long)&lock.list;
		exp &= ~0xFFFFFFFFULL;
	}

	ASSERT_EQ((uintptr_t)(unsigned long)head.list_op_pending, exp);
}

/*
 * 32-bit version of child_lock_holder.
 */
static int child_lock_holder32(void *arg)
{
	struct child_args *cargs = arg;
	struct lock_struct32 *locks = cargs->arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct robust_list_head32 *head;
	pid_t tid = gettid();
	int i, ret;

	head = mmap((void *)0x10000, sizeof(*head), PROT_READ | PROT_WRITE,
		    MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	ASSERT_TRUE((uintptr_t) head);
	ASSERT_LT(((uint32_t)(uintptr_t) head), 0x7FFFFFFF);

	head->futex_offset = (uint32_t) ((size_t) offsetof(struct lock_struct32, futex) -
			     (size_t) offsetof(struct lock_struct32, list));
	head->list.next = (uint32_t)(uintptr_t) &head->list;
	head->list_op_pending = (uint32_t)(uintptr_t) NULL;

	ret = set_robust_list2((struct robust_list_head *) head,
			FUTEX_ROBUST_LIST_CMD_CREATE_32, 0, 0);
	ASSERT_GE(ret, 0);

	/*
	 * Take all the locks and insert them in the list
	 */
	for (i = 0; i < CHILD_NR; i++) {
		struct robust_list32 *list = &head->list;

		locks[i].futex = tid;

		while (list->next != (uint32_t)(uintptr_t) &head->list)
			list = (struct robust_list32 *)(uintptr_t) list->next;

		list->next = (uint32_t)(uintptr_t) &locks[i].list;
		locks[i].list.next = (uint32_t)(uintptr_t) &head->list;
	}

	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier2);

	/* See comment at child_fn_lock() */
	usleep(SLEEP_US);

	/* Exit holding all the locks */
	return 0;
}

static int child_wait_lock32(void *arg)
{
	struct child_args *cargs = arg;
	struct __test_metadata *_metadata = cargs->_metadata;
	struct lock_struct32 *lock = cargs->arg;
	atomic_futex_t *futex;
	struct timespec to;
	pid_t tid;
	int ret;

	futex = &lock->futex;

	pthread_barrier_wait(&barrier2);

	to.tv_sec = FUTEX_TIMEOUT;
	to.tv_nsec = 0;

	tid = atomic_load(futex);

	/* Kernel ignores futexes without the waiters flag */
	tid |= FUTEX_WAITERS;
	atomic_store(futex, tid);

	ret = futex_wait((futex_t *) futex, tid, &to, 0);

	ASSERT_EQ(ret, 0);
	ASSERT_TRUE(lock->futex & FUTEX_OWNER_DIED);

	return 0;
}

/*
 * Test to create a 32-bit robust list in a 64-bit kernel. Replicate
 * test_robust_list_multiple_elements, but it's simplified: don't do all the
 * mutex lock dance, just insert futexes in the list and check if the kernel
 * correctly walks the list and wake the threads
 */
TEST(test_32bit_lists)
{
	struct lock_struct32 *locks;
	pid_t pids[CHILD_NR + 1];
	int i, ret, wstatus;

#ifndef BUILD_64
	SKIP(return, "Test only for 64-bit\n");
#endif

	if (!robust_list2_support())
		SKIP(return, "robust_list2 not supported\n");

	locks = mmap((void *)0x20000, sizeof(*locks) * CHILD_NR,
		     PROT_READ | PROT_WRITE,
		     MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS,
		     -1, 0);

	ASSERT_NE(locks, NULL);
	ASSERT_LT((uintptr_t) locks, 0x7FFFFFFF);

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);
	ret = pthread_barrier_init(&barrier2, NULL, CHILD_NR + 1);
	ASSERT_EQ(ret, 0);

	pids[0] = create_child(_metadata, &child_lock_holder32, locks);

	/* Wait until the locker thread takes the look */
	pthread_barrier_wait(&barrier);

	for (i = 0; i < CHILD_NR; i++)
		pids[i+1] = create_child(_metadata, &child_wait_lock32, &locks[i]);

	/* Wait for all children to return */
	ret = 0;

	for (i = 0; i < CHILD_NR; i++) {
		waitpid(pids[i], &wstatus, 0);
		if (WEXITSTATUS(wstatus))
			ret = -1;
	}

	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	munmap(locks, sizeof(*locks) * CHILD_NR);
}

TEST_HARNESS_MAIN
