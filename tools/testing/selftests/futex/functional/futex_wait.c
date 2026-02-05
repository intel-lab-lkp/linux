// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright Collabora Ltd., 2021
 *
 * futex cmp requeue test by André Almeida <andrealmeid@collabora.com>
 */

#include <pthread.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "futextest.h"
#include "kselftest_harness.h"

#define timeout_ns  30000000
#define WAKE_WAIT_US 10000
#define SHM_PATH "futex_shm_file"

void *futex;

static void *waiterfn(void *arg)
{
	struct timespec to;
	unsigned int flags = 0;

	if (arg)
		flags = *((unsigned int *) arg);

	to.tv_sec = 0;
	to.tv_nsec = timeout_ns;

	if (futex_wait(futex, 0, &to, flags))
		printf("waiter failed errno %d\n", errno);

	return NULL;
}

TEST(private_futex)
{
	unsigned int flags = FUTEX_PRIVATE_FLAG;
	u_int32_t f_private = 0;
	pthread_t waiter;

	futex = &f_private;

	/* Testing a private futex */
	ASSERT_EQ(0, pthread_create(&waiter, NULL, waiterfn, (void *) &flags));

	usleep(WAKE_WAIT_US);

	EXPECT_EQ(1, futex_wake(futex, 1, FUTEX_PRIVATE_FLAG));
}

TEST(anon_page)
{
	u_int32_t *shared_data;
	pthread_t waiter;
	int shm_id;

	/* Testing an anon page shared memory */
	shm_id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
	if (shm_id < 0 && errno == ENOSYS)
		SKIP(return, "shmget syscall not supported");
	ASSERT_LE(0, shm_id);

	shared_data = shmat(shm_id, NULL, 0);

	*shared_data = 0;
	futex = shared_data;

	ASSERT_EQ(0, pthread_create(&waiter, NULL, waiterfn, NULL));

	usleep(WAKE_WAIT_US);

	EXPECT_EQ(1, futex_wake(futex, 1, 0));

	shmdt(shared_data);
}

TEST(file_backed)
{
	u_int32_t f_private = 0;
	pthread_t waiter;
	int fd;
	void *shm;

	/* Testing a file backed shared memory */
	ASSERT_LE(0, (fd = open(SHM_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)));
	ASSERT_EQ(0, ftruncate(fd, sizeof(f_private)));

	shm = mmap(NULL, sizeof(f_private), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT_NE(MAP_FAILED, shm);

	memcpy(shm, &f_private, sizeof(f_private));

	futex = shm;

	ASSERT_EQ(0, pthread_create(&waiter, NULL, waiterfn, NULL));

	usleep(WAKE_WAIT_US);

	EXPECT_EQ(1, futex_wake(shm, 1, 0));

	munmap(shm, sizeof(f_private));
	remove(SHM_PATH);
	close(fd);
}

TEST_HARNESS_MAIN
