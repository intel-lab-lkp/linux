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
#include "../../kselftest_harness.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define STACK_SIZE (1024 * 1024)

#define FUTEX_TIMEOUT 3

#define SLEEP_US 100

#ifndef SYS_set_robust_list2
# define SYS_set_robust_list2 470
# define SYS_get_robust_list2 471

enum robust_list_cmd {
	FUTEX_ROBUST_LIST_CMD_SET_64,
	FUTEX_ROBUST_LIST_CMD_SET_32,
	FUTEX_ROBUST_LIST_CMD_LIST_LIMIT,
	FUTEX_ROBUST_LIST_CMD_USER_MAX,
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

static int set_robust_list2(struct robust_list_head *head, int index,
			    enum robust_list_cmd cmd, unsigned int flags)
{
	return syscall(SYS_set_robust_list2, head, index, cmd, flags);
}

static int get_robust_list2(int pid, struct robust_list_head **head,
			    unsigned int index, unsigned int flags)
{
	return syscall(SYS_get_robust_list2, pid, head, index, flags);
}

static bool robust_list2_support(void)
{
	int ret = set_robust_list2(0, 0, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0);

	if (ret == -1 && errno == ENOSYS)
		return false;

	return true;
}

/*
 * Return the set command according to the app bitness
 */
static int get_cmd_set(void)
{
	return sizeof(uintptr_t) == 8 ? FUTEX_ROBUST_LIST_CMD_SET_64 :
	       FUTEX_ROBUST_LIST_CMD_SET_32;
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

	ASSERT_NE(robust_list2_support(), false);
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
struct lock_struct {
	_Atomic(unsigned int)	futex;
	struct robust_list	list;
	bool			robust2;
};

struct lock_struct32 {
	_Atomic(uint32_t)	futex;
	struct robust_list32	list;
};

/*
 * Helper function to spawn a child thread. Returns -1 on error, pid on success
 */
static int create_child(int (*fn)(void *arg), void *arg)
{
	char *stack;
	pid_t pid;

	stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED)
		return -1;

	stack += STACK_SIZE;

	pid = clone(fn, stack, CLONE_VM | SIGCHLD, arg);

	if (pid == -1)
		return -1;

	return pid;
}

/*
 * Helper function to prepare and register a robust list
 */
static int set_list(struct robust_list_head *head, bool robust2, int index)
{
	head->futex_offset = (size_t) offsetof(struct lock_struct, futex) -
			     (size_t) offsetof(struct lock_struct, list);
	head->list.next = &head->list;
	head->list_op_pending = NULL;

	if (!robust2)
		return set_robust_list(head, sizeof(*head));

	return set_robust_list2(head, index, get_cmd_set(), 0);
}

static int get_list(pid_t pid, struct robust_list_head **head, bool robust2, int index)
{
	int ret;

	if (!robust2) {
		size_t len_ptr;

		ret = get_robust_list(pid, head, &len_ptr);
		if (sizeof(**head) != len_ptr)
			return -EINVAL;

		return ret;
	}

	return get_robust_list2(pid, head, index, 0);
}

/*
 * A basic (and incomplete) mutex lock function with robustness
 */
static int mutex_lock(struct lock_struct *lock, struct robust_list_head *head, bool error_inject)
{
	_Atomic(unsigned int) *futex = &lock->futex;
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
	struct lock_struct *lock = arg;
	struct robust_list_head head;
	int ret;

	ret = set_list(&head, lock->robust2, 0);
	if (ret) {
		ksft_test_result_fail("set_robust_list error\n");
		return ret;
	}

	ret = mutex_lock(lock, &head, false);
	if (ret) {
		ksft_test_result_fail("mutex_lock error\n");
		return ret;
	}

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
	_Atomic(unsigned int) *futex = &lock.futex;
	int ret, pid, wstatus;
	struct robust_list_head head;

	lock.robust2 = variant->robust2;

	ret = set_list(&head, lock.robust2, 0);
	ASSERT_EQ(ret, 0);

	/*
	 * Lets use a barrier to ensure that the child thread takes the lock
	 * before the parent
	 */
	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);

	pid = create_child(&child_fn_lock, &lock);
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

	/* Pass only if the child hasn't return error */
	if (!WEXITSTATUS(wstatus))
		ksft_test_result_pass("%s\n", __func__);
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

	ksft_test_result_pass("%s\n", __func__);
}

/*
 * Test invalid parameters
 */
TEST(test_set_robust_list2_inval)
{
	struct robust_list_head head;
	int ret, list_limit;

	if (!robust_list2_support()) {
		ksft_test_result_skip("robust_list2 not supported\n");
		return;
	}

	/* Bad flag */
	ret = set_robust_list2(&head, 0, get_cmd_set(), 999);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);

	/* Bad index */
	list_limit = set_robust_list2(NULL, 0, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0);
	ASSERT_GT(list_limit, 0);

	ret = set_robust_list2(&head, -1, get_cmd_set(), 0);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);

	ret = set_robust_list2(&head, list_limit + 1, get_cmd_set(), 0);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);

	/* Bad command */
	ret = set_robust_list2(&head, 0, FUTEX_ROBUST_LIST_CMD_USER_MAX, 0);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);

	ret = set_robust_list2(&head, 0, -1, 0);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);
}

/*
 * Test get_robust_list with pid = 0, getting the list of the running thread
 */
TEST_F(robust_api, test_get_robust_list_self)
{
	struct robust_list_head head, head2, *get_head;
	bool robust2 = variant->robust2;
	int ret;

	ret = set_list(&head, robust2, 0);
	ASSERT_EQ(ret, 0);

	ret = get_list(0, &get_head, robust2, 0);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(get_head, &head);

	ret = set_list(&head2, robust2, 0);
	ASSERT_EQ(ret, 0);

	ret = get_list(0, &get_head, robust2, 0);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(get_head, &head2);

	ksft_test_result_pass("%s\n", __func__);
}

struct child_arg_struct {
	struct robust_list_head *head;
	bool robust2;
};

static int child_list(void *arg)
{
	struct child_arg_struct *child = arg;
	struct robust_list_head *head;
	bool robust2 = child->robust2;
	int ret;

	head = child->head;

	ret = set_list(head, robust2, 0);
	if (ret) {
		ksft_test_result_fail("set_robust_list error\n");
		return -1;
	}

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
TEST_F(robust_api, test_get_robust_list_child)
{
	struct robust_list_head head, *get_head;
	bool robust2 = variant->robust2;
	struct child_arg_struct child =
		{.robust2 = robust2, .head = &head};
	int ret, wstatus;
	pid_t tid;


	ret = pthread_barrier_init(&barrier, NULL, 2);
	ret = pthread_barrier_init(&barrier2, NULL, 2);
	ASSERT_EQ(ret, 0);

	tid = create_child(&child_list, &child);
	ASSERT_NE(tid, -1);

	pthread_barrier_wait(&barrier);

	ret = get_list(tid, &get_head, robust2, 0);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(&head, get_head);

	pthread_barrier_wait(&barrier2);

	wait(&wstatus);
	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	/* Pass only if the child hasn't return error */
	if (!WEXITSTATUS(wstatus))
		ksft_test_result_pass("%s\n", __func__);
}

static int child_fn_lock_with_error(void *arg)
{
	struct lock_struct *lock = arg;
	struct robust_list_head head;
	int ret;

	ret = set_list(&head, false, 0);
	if (ret) {
		ksft_test_result_fail("set_robust_list error\n");
		return -1;
	}

	ret = mutex_lock(lock, &head, true);
	if (ret) {
		ksft_test_result_fail("mutex_lock error\n");
		return -1;
	}

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
	_Atomic(unsigned int) *futex = &lock.futex;
	int ret, wstatus;
	struct robust_list_head head;

	lock.robust2 = variant->robust2;

	ret = set_list(&head, lock.robust2, 0);
	ASSERT_EQ(ret, 0);

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);

	ret = create_child(&child_fn_lock_with_error, &lock);
	ASSERT_NE(ret, -1);

	pthread_barrier_wait(&barrier);
	ret = mutex_lock(&lock, &head, false);

	ASSERT_EQ(ret, 0);

	ASSERT_TRUE(*futex & FUTEX_OWNER_DIED);

	wait(&wstatus);
	pthread_barrier_destroy(&barrier);

	/* Pass only if the child hasn't return error */
	if (!WEXITSTATUS(wstatus))
		ksft_test_result_pass("%s\n", __func__);
	else
		ksft_test_result_fail("%s\n", __func__);
}

#define CHILD_NR 10

static int child_lock_holder(void *arg)
{
	struct lock_struct *locks = arg;
	struct robust_list_head head;
	int i;

	set_list(&head, locks[0].robust2, 0);

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
	struct lock_struct *lock = arg;
	struct robust_list_head head;
	int ret;

	pthread_barrier_wait(&barrier2);
	ret = mutex_lock(lock, &head, false);

	if (ret) {
		ksft_test_result_fail("mutex_lock error\n");
		return -1;
	}

	if (!(lock->futex & FUTEX_OWNER_DIED)) {
		ksft_test_result_fail("futex not marked with FUTEX_OWNER_DIED\n");
		return -1;
	}

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

	if (!robust_list2_support()) {
		ksft_test_result_skip("robust_list2 not supported\n");
		return;
	}

	locks[0].robust2 = variant->robust2;

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);
	ret = pthread_barrier_init(&barrier2, NULL, CHILD_NR + 1);
	ASSERT_EQ(ret, 0);

	pids[0] = create_child(&child_lock_holder, &locks);

	/* Wait until the locker thread takes the look */
	pthread_barrier_wait(&barrier);

	for (i = 0; i < CHILD_NR; i++)
		pids[i+1] = create_child(&child_wait_lock, &locks[i]);

	/* Wait for all children to return */
	ret = 0;

	for (i = 0; i < CHILD_NR; i++) {
		waitpid(pids[i], &wstatus, 0);
		if (WEXITSTATUS(wstatus))
			ret = -1;
	}

	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	/* Pass only if the child hasn't return error */
	if (!ret)
		ksft_test_result_pass("%s\n", __func__);
}

static int child_lock_holder_multiple_lists(void *arg)
{
	struct lock_struct *locks = arg;
	struct robust_list_head *heads;
	int i, list_limit;

	list_limit = set_robust_list2(NULL, 0, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0);

	heads = malloc(list_limit * sizeof(*heads));
	if (!heads)
		return -1;

	for (i = 0; i < list_limit; i++) {
		set_list(&heads[i], true, i);
		locks[i].futex = 0;
		mutex_lock(&locks[i], &heads[i], false);
	}

	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier2);

	/* See comment at child_fn_lock() */
	usleep(SLEEP_US);

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

	if (!robust_list2_support()) {
		ksft_test_result_skip("robust_list2 not supported\n");
		return;
	}

	list_limit = set_robust_list2(NULL, 0, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0);
	ASSERT_GT(list_limit, 1);

	locks = malloc(list_limit * sizeof(*locks));
	ASSERT_NE(locks, NULL);

	pids = malloc(list_limit * sizeof(*pids));
	ASSERT_NE(pids, NULL);

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);
	ret = pthread_barrier_init(&barrier2, NULL, list_limit + 1);
	ASSERT_EQ(ret, 0);

	pids[0] = create_child(&child_lock_holder_multiple_lists, locks);

	/* Wait until the locker thread takes the look */
	pthread_barrier_wait(&barrier);

	for (i = 0; i < list_limit; i++)
		pids[i+1] = create_child(&child_wait_lock, &locks[i]);

	/* Wait for all children to return */
	ret = 0;

	for (i = 0; i < list_limit; i++) {
		waitpid(pids[i], &wstatus, 0);
		if (WEXITSTATUS(wstatus))
			ret = -1;
	}

	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	/* Pass only if the child hasn't return error */
	if (!ret)
		ksft_test_result_pass("%s\n", __func__);

	free(locks);
	free(pids);
}

static int child_circular_list(void *arg)
{
	struct robust_list_head head;
	struct lock_struct a, b, c;
	bool robust2 = *(bool *) arg;
	int ret;

	ret = set_list(&head, robust2, 0);
	if (ret) {
		ksft_test_result_fail("set_list error\n");
		return -1;
	}

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
	int wstatus;
	bool robust2 = variant->robust2;

	create_child(child_circular_list, &robust2);

	wait(&wstatus);

	/* Pass only if the child hasn't return error */
	if (!WEXITSTATUS(wstatus))
		ksft_test_result_pass("%s\n", __func__);
}

/*
 * 32-bit version of child_lock_holder. 
 */
static int child_lock_holder32(void *arg)
{
	struct lock_struct32 *locks = arg;
	struct robust_list_head32 *head;
	pid_t tid = gettid();
	int i, ret;

	head = mmap((void *)0x10000, sizeof(*head), PROT_READ | PROT_WRITE,
		    MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (!head || ((uint32_t)(uintptr_t) head) > 0x7FFFFFFF) {
		ksft_test_result_fail("child_lock_holder32 error\n");
		return -1;
	}

	head->futex_offset = (uint32_t) ((size_t) offsetof(struct lock_struct32, futex) -
			     (size_t) offsetof(struct lock_struct32, list));
	head->list.next = (uint32_t)(uintptr_t) &head->list;
	head->list_op_pending = (uint32_t)(uintptr_t) NULL;

	ret = set_robust_list2((struct robust_list_head *) head, 0,
			       FUTEX_ROBUST_LIST_CMD_SET_32, 0);
	if (ret) {
		ksft_test_result_fail("set_robust_list2 error\n");
		return -1;
	}

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
	struct lock_struct32 *lock = arg;
	_Atomic(unsigned int) *futex;
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

	if (ret) {
		ksft_test_result_fail("futex_wait error\n");
		return -1;
	}

	if (!(lock->futex & FUTEX_OWNER_DIED)) {
		ksft_test_result_fail("futex not marked with FUTEX_OWNER_DIED\n");
		return -1;
	}

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

	if (sizeof(uintptr_t) != 8) {
		ksft_test_result_skip("Test only for 64-bit\n");
		return;
	}

	if (!robust_list2_support()) {
		ksft_test_result_skip("robust_list2 not supported\n");
		return;
	}

	locks = mmap((void *)0x20000, sizeof(*locks) * CHILD_NR,
		     PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS,
		     -1, 0);

	ASSERT_NE(locks, NULL);
	ASSERT_LT((uintptr_t) locks, 0x7FFFFFFF);

	ret = pthread_barrier_init(&barrier, NULL, 2);
	ASSERT_EQ(ret, 0);
	ret = pthread_barrier_init(&barrier2, NULL, CHILD_NR + 1);
	ASSERT_EQ(ret, 0);

	pids[0] = create_child(&child_lock_holder32, locks);

	/* Wait until the locker thread takes the look */
	pthread_barrier_wait(&barrier);

	for (i = 0; i < CHILD_NR; i++)
		pids[i+1] = create_child(&child_wait_lock32, &locks[i]);

	/* Wait for all children to return */
	ret = 0;

	for (i = 0; i < CHILD_NR; i++) {
		waitpid(pids[i], &wstatus, 0);
		if (WEXITSTATUS(wstatus))
			ret = -1;
	}

	pthread_barrier_destroy(&barrier);
	pthread_barrier_destroy(&barrier2);

	/* Pass only if the child hasn't return error */
	if (!ret)
		ksft_test_result_pass("%s\n", __func__);

	munmap(locks, sizeof(*locks) * CHILD_NR);
}

/*
 * Test setting and getting mutiples head lists
 */
TEST(set_and_get_robust2)
{
	struct robust_list_head *head = NULL, *heads;
	int i, list_limit, ret;

	if (!robust_list2_support()) {
		ksft_test_result_skip("robust_list2 not supported\n");
		return;
	}

	list_limit = set_robust_list2(NULL, 0, FUTEX_ROBUST_LIST_CMD_LIST_LIMIT, 0);

	heads = malloc(list_limit * sizeof(*heads));
	ASSERT_NE(heads, NULL);

	for (i = 0; i < list_limit; i++) {
		ret = set_list(&heads[i], true, i);
		ASSERT_EQ(ret, 0);
	}

	for (i = 0; i < list_limit; i++) {
		ret = get_list(0, &head, true, i);
		ASSERT_EQ(ret, 0);
		ASSERT_EQ(head, &heads[i]);
	}

	free(heads);
	ksft_test_result_pass("%s\n", __func__);
}

TEST_HARNESS_MAIN
