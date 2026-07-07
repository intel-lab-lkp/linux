// SPDX-License-Identifier: GPL-2.0
/*
 * hugetlb_surplus_mempolicy
 *
 * Reserving surplus hugepages within mempolicies is quite tricky due to
 * the transient nature of cpusets and mempolicies. As such, these tests
 * do not cover all edge cases, but rather focus on what the kernel can
 * currently do to reserve surplus hugepages in the presence of cpusets
 * and mempolicies to help check for regressions in this behavior.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <numa.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "vm_util.h"
#include "kselftest.h"

#define HPSIZE_BYTES default_huge_page_size()
#define HPSIZE_KB default_huge_page_size() >> 10
#define GLOBAL_SYS_HP_PATH "/sys/kernel/mm/hugepages/hugepages-%lukB/%s"
#define NODE_SYS_HP_PATH "/sys/devices/system/node/node%u/hugepages/hugepages-%lukB/%s"

struct bitmask **nodemasks;
int *nodeids;

pthread_t *threads;
struct thread_args {
	struct bitmask *my_nodemask;
	int to_reserve;
};
struct thread_args *per_thread_args;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int wake_cond;

char *nr_overcommit_hugepages_path;
char *g_free_hugepages_path;
char *g_nr_hugepages_path;
char *g_resv_hugepages_path;
char *g_surplus_hugepages_path;
char *n0_free_hugepages_path;
char *n0_nr_hugepages_path;
char *n0_surplus_hugepages_path;
char *n1_free_hugepages_path;
char *n1_nr_hugepages_path;
char *n1_surplus_hugepages_path;

unsigned long g_free_hugepages, g_nr_hugepages;
unsigned long g_resv_hugepages, g_surplus_hugepages;
unsigned long n0_free_hugepages, n0_nr_hugepages, n0_surplus_hugepages;
unsigned long n1_free_hugepages, n1_nr_hugepages, n1_surplus_hugepages;
unsigned long orig_n0_nr_hugepages, orig_n1_nr_hugepages;
unsigned long orig_nr_overcommit_hugepages;


/* setup_paths
 *
 * Helper function to create strings for the various hugetlb page sysfs
 * paths. The strings are used to read from and write to the sysfs files.
 */
static void setup_paths(void)
{
	asprintf(&nr_overcommit_hugepages_path,
			"/proc/sys/vm/nr_overcommit_hugepages");
	asprintf(&g_free_hugepages_path, GLOBAL_SYS_HP_PATH,
			HPSIZE_KB, "free_hugepages");
	asprintf(&g_nr_hugepages_path, GLOBAL_SYS_HP_PATH,
			HPSIZE_KB, "nr_hugepages");
	asprintf(&g_resv_hugepages_path, GLOBAL_SYS_HP_PATH,
			HPSIZE_KB, "resv_hugepages");
	asprintf(&g_surplus_hugepages_path, GLOBAL_SYS_HP_PATH,
			HPSIZE_KB, "surplus_hugepages");
	asprintf(&n0_free_hugepages_path, NODE_SYS_HP_PATH, nodeids[0],
			HPSIZE_KB, "free_hugepages");
	asprintf(&n0_nr_hugepages_path, NODE_SYS_HP_PATH, nodeids[0],
			HPSIZE_KB, "nr_hugepages");
	asprintf(&n0_surplus_hugepages_path, NODE_SYS_HP_PATH, nodeids[0],
			HPSIZE_KB, "surplus_hugepages");
	asprintf(&n1_free_hugepages_path, NODE_SYS_HP_PATH, nodeids[1],
			HPSIZE_KB, "free_hugepages");
	asprintf(&n1_nr_hugepages_path, NODE_SYS_HP_PATH, nodeids[1],
			HPSIZE_KB, "nr_hugepages");
	asprintf(&n1_surplus_hugepages_path, NODE_SYS_HP_PATH, nodeids[1],
			HPSIZE_KB, "surplus_hugepages");
}

/* get_hugepage_stats
 *
 * Helper function to simply grab a bunch of the hugetlb page metrics in sysfs
 */
static void get_hugepage_stats(void)
{
	read_sysfs(g_free_hugepages_path, &g_free_hugepages);
	read_sysfs(g_nr_hugepages_path, &g_nr_hugepages);
	read_sysfs(g_resv_hugepages_path, &g_resv_hugepages);
	read_sysfs(g_surplus_hugepages_path, &g_surplus_hugepages);
	read_sysfs(n0_free_hugepages_path, &n0_free_hugepages);
	read_sysfs(n0_nr_hugepages_path, &n0_nr_hugepages);
	read_sysfs(n0_surplus_hugepages_path, &n0_surplus_hugepages);
	read_sysfs(n1_free_hugepages_path, &n1_free_hugepages);
	read_sysfs(n1_nr_hugepages_path, &n1_nr_hugepages);
	read_sysfs(n1_surplus_hugepages_path, &n1_surplus_hugepages);
}

/* save_hugepage_configs
 *
 * Helper function to save the current state of the hugepage configs so this
 * test suite doesn't clobber configs needed for other tests.
 */
static void save_hugepage_configs(void)
{
	read_sysfs(n0_nr_hugepages_path, &orig_n0_nr_hugepages);
	read_sysfs(n1_nr_hugepages_path, &orig_n1_nr_hugepages);
	read_sysfs(nr_overcommit_hugepages_path, &orig_nr_overcommit_hugepages);
}

/* restore_hugepage_configs
 *
 * Helper function to restore the state of hugepage configs before this test
 * was ran.
 */
static void restore_hugepage_configs(void)
{
	write_sysfs(n0_nr_hugepages_path, orig_n0_nr_hugepages);
	write_sysfs(n1_nr_hugepages_path, orig_n1_nr_hugepages);
	write_sysfs(nr_overcommit_hugepages_path, orig_nr_overcommit_hugepages);
}

/* reset_hugepages
 *
 * Helper function to reset static hugetlb page reservations to 0.
 * Used to get back to a clear state between tests.
 */
static void reset_hugepages(void)
{
	write_sysfs(nr_overcommit_hugepages_path, 0);
	write_sysfs(g_nr_hugepages_path, 0);
	write_sysfs(n0_nr_hugepages_path, 0);
	write_sysfs(n1_nr_hugepages_path, 0);
}

/* can_run
 *
 * Does sanity checking first to make sure the tests can even run.
 */
static void check_requirements(void)
{
	if (geteuid() != 0)
		ksft_exit_skip("Please run the test as root.\n");

	if (numa_available() == -1)
		ksft_exit_skip("Numa is unavailable.\n");

	if (numa_num_configured_nodes() < 2)
		ksft_exit_skip("Not enough nodes to test.\n");

	if (numa_num_task_nodes() < 2)
		ksft_exit_skip("Current mempolicy is too restrictive.\n");
}

static void cleanup(char *err_msg)
{
	free(per_thread_args);
	free(threads);
	free(nodeids);
	free(nodemasks);
	free(nr_overcommit_hugepages_path);
	free(g_free_hugepages_path);
	free(g_nr_hugepages_path);
	free(g_resv_hugepages_path);
	free(g_surplus_hugepages_path);
	free(n0_free_hugepages_path);
	free(n0_nr_hugepages_path);
	free(n0_surplus_hugepages_path);
	free(n1_free_hugepages_path);
	free(n1_nr_hugepages_path);
	free(n1_surplus_hugepages_path);
	if (err_msg)
		ksft_exit_fail_msg(err_msg);
}

/* setup_node_info
 *
 * Creates the bitmasks used to isolate test runners and their hugetlb page
 * reservations.
 */
static void setup_node_info(void)
{
	int i;
	int ith_nodemask = 0;

	nodeids = calloc(2, sizeof(int));
	nodemasks = calloc(2, sizeof(struct bitmask *));

	if (!nodemasks || !nodeids)
		cleanup("Failed to allocate nodemasks or nodeids.");

	/* Walk the nodes available to us. Create two bitmasks, one of the
	 * index of the first node available to us, and the second of the next
	 * node available to us.
	 */
	for (i = 0; i < numa_num_task_nodes(); i++) {
		if (numa_bitmask_isbitset(numa_get_mems_allowed(), i)) {
			nodeids[ith_nodemask] = i;
			nodemasks[ith_nodemask++] = numa_bitmask_setbit(
					numa_allocate_nodemask(), i);
		}
	}
	if (ith_nodemask != 2 || !nodemasks[0] || !nodemasks[1])
		cleanup("Failed to create nodemasks.");
}

/* setup_threads
 *
 * Helper function to setup space for threads.
 */
static void setup_threads(void)
{
	per_thread_args = calloc(2, sizeof(struct thread_args));
	if (!per_thread_args)
		cleanup("calloc thread args.");

	threads = calloc(2, sizeof(pthread_t));
	if (!threads)
		cleanup("calloc threads.");
}

/* reserve_hugepage
 *
 * Helper function to reserve a hugetlb page
 */
static unsigned long *reserve_hugepage(void)
{
	return (unsigned long *) mmap(NULL, HPSIZE_BYTES, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
}

/* thread_work
 *
 * Test runners. Performs the work of reserving and freeing hugetlb pages.
 */
static void *thread_work(void *arg)
{
	struct thread_args *t_args = (struct thread_args *) arg;
	unsigned long **hugepages;
	int i;

	hugepages = (unsigned long **) calloc(t_args->to_reserve,
						sizeof(unsigned long **));

	/* Reserve hugetlb pages on my node */
	if (t_args->my_nodemask)
		numa_bind(t_args->my_nodemask);
	for (i = 0; i < t_args->to_reserve; i++) {
		hugepages[i] = reserve_hugepage();
		/* Tests may purposefully try to overallocate, so just
		 * fallthrough rather than error out
		 */
		if (hugepages[i] == MAP_FAILED) {
			t_args->to_reserve = i;
			break;
		}
	}

	/* Go to sleep until main thread wakes us up */
	pthread_mutex_lock(&mutex);
	while (!wake_cond)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);

	/* Try to free those hugetlb pages */
	for (i = 0; i < t_args->to_reserve; i++) {
		if (munmap(hugepages[i], HPSIZE_BYTES) < 0)
			ksft_perror("munmap() failed! Check for leaked hugetlb pages!\n");
	}
	free(hugepages);
	return NULL;
}

/* wake_children
 *
 * Helper function to wake children threads.
 */
static void wake_children(void)
{
	pthread_mutex_lock(&mutex);
	wake_cond = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
}

/* test1
 *
 * Sanity checking, attempt to reserve a surplus hugetlb page anywhere.
 */
static void test1(void)
{
	reset_hugepages();

	write_sysfs(nr_overcommit_hugepages_path, 1);
	per_thread_args[0].my_nodemask = NULL;
	per_thread_args[0].to_reserve = 1;

	pthread_create(&threads[0], NULL, thread_work, &per_thread_args[0]);

	usleep(500000);

	get_hugepage_stats();
	ksft_test_result((g_free_hugepages == 1 && g_nr_hugepages == 1 &&
			 g_resv_hugepages == 1 && g_surplus_hugepages == 1) &&
			 ((n0_free_hugepages == 1 && n0_nr_hugepages == 1 &&
			 n0_surplus_hugepages == 1 && n1_free_hugepages == 0 &&
			 n1_nr_hugepages == 0 && n1_surplus_hugepages == 0) ||
			 (n0_free_hugepages == 0 && n0_nr_hugepages == 0 &&
			 n0_surplus_hugepages == 0 && n1_free_hugepages == 1 &&
			 n1_nr_hugepages == 1 && n1_surplus_hugepages == 1)),
			 "Reserve 1 surplus hugepage anywhere\n");

	wake_children();
	pthread_join(threads[0], NULL);
	wake_cond = 0;
	reset_hugepages();
}

/* test2
 *
 * Sanity checking, attempt to reserve a surplus hugetlb page with
 * a mempolicy.
 */
static void test2(void)
{
	reset_hugepages();

	write_sysfs(nr_overcommit_hugepages_path, 1);
	per_thread_args[0].my_nodemask = nodemasks[0];
	per_thread_args[0].to_reserve = 1;

	pthread_create(&threads[0], NULL, thread_work, &per_thread_args[0]);

	usleep(500000);

	get_hugepage_stats();
	ksft_test_result(g_free_hugepages == 1 && g_nr_hugepages == 1 &&
			 g_resv_hugepages == 1 && g_surplus_hugepages == 1 &&
			 n0_free_hugepages == 1 && n0_nr_hugepages == 1 &&
			 n0_surplus_hugepages == 1 && n1_free_hugepages == 0 &&
			 n1_nr_hugepages == 0 && n1_surplus_hugepages == 0,
			 "Reserve 1 surplus hugepage on node0\n");

	wake_children();
	pthread_join(threads[0], NULL);
	wake_cond = 0;
	reset_hugepages();
}

/* test3
 *
 * Set a static hugepage and reserve off node
 */
static void test3(void)
{
	reset_hugepages();

	write_sysfs(nr_overcommit_hugepages_path, 1);
	write_sysfs(n0_nr_hugepages_path, 1);

	per_thread_args[0].my_nodemask = nodemasks[0];
	per_thread_args[0].to_reserve = 0;
	per_thread_args[1].my_nodemask = nodemasks[1];
	per_thread_args[1].to_reserve = 1;

	pthread_create(&threads[0], NULL, thread_work, &per_thread_args[0]);
	pthread_create(&threads[1], NULL, thread_work, &per_thread_args[1]);

	usleep(500000);

	get_hugepage_stats();
	ksft_test_result(g_free_hugepages == 2 && g_nr_hugepages == 2 &&
			 g_resv_hugepages == 1 && g_surplus_hugepages == 1 &&
			 n0_free_hugepages == 1 && n0_nr_hugepages == 1 &&
			 n0_surplus_hugepages == 0 && n1_free_hugepages == 1 &&
			 n1_nr_hugepages == 1 && n1_surplus_hugepages == 1,
			 "Set 1 static hugepage on node0, reserve surplus hugepage on node 1\n");

	wake_children();
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	wake_cond = 0;
	reset_hugepages();
}

/* test4
 *
 * Reserve static hugepage on node0, reserve surplus hugepage on node1
 */
static void test4(void)
{
	reset_hugepages();

	write_sysfs(nr_overcommit_hugepages_path, 1);
	write_sysfs(n0_nr_hugepages_path, 1);

	per_thread_args[0].my_nodemask = nodemasks[0];
	per_thread_args[0].to_reserve = 1;
	per_thread_args[1].my_nodemask = nodemasks[1];
	per_thread_args[1].to_reserve = 1;

	pthread_create(&threads[0], NULL, thread_work, &per_thread_args[0]);
	pthread_create(&threads[1], NULL, thread_work, &per_thread_args[1]);

	usleep(500000);

	get_hugepage_stats();
	ksft_test_result(g_free_hugepages == 2 && g_nr_hugepages == 2 &&
			 g_resv_hugepages == 2 && g_surplus_hugepages == 1 &&
			 n0_free_hugepages == 1 && n0_nr_hugepages == 1 &&
			 n0_surplus_hugepages == 0 && n1_free_hugepages == 1 &&
			 n1_nr_hugepages == 1 && n1_surplus_hugepages == 1,
			 "Reserve 1 static hugepage on node0, reserve surplus hugepage on node 1\n");

	wake_children();
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	wake_cond = 0;
	reset_hugepages();
}

/* test5
 *
 * Reserve static hugepage on node0, reserve surplus hugepage on node1 and
 * fail to over allocate another.
 */
static void test5(void)
{
	reset_hugepages();

	write_sysfs(nr_overcommit_hugepages_path, 1);
	write_sysfs(n0_nr_hugepages_path, 1);

	per_thread_args[0].my_nodemask = nodemasks[0];
	per_thread_args[0].to_reserve = 1;
	per_thread_args[1].my_nodemask = nodemasks[1];
	per_thread_args[1].to_reserve = 2;

	pthread_create(&threads[0], NULL, thread_work, &per_thread_args[0]);
	pthread_create(&threads[1], NULL, thread_work, &per_thread_args[1]);

	usleep(500000);

	get_hugepage_stats();
	ksft_test_result(g_free_hugepages == 2 && g_nr_hugepages == 2 &&
			 g_resv_hugepages == 2 && g_surplus_hugepages == 1 &&
			 n0_free_hugepages == 1 && n0_nr_hugepages == 1 &&
			 n0_surplus_hugepages == 0 && n1_free_hugepages == 1 &&
			 n1_nr_hugepages == 1 && n1_surplus_hugepages == 1,
			 "Intentionally overallocate and fail due to nr_overcommit_hugepages limit.\n");

	wake_children();
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	wake_cond = 0;
	reset_hugepages();

}

int main(void)
{
	ksft_print_header();
	ksft_set_plan(5);

	check_requirements();
	setup_threads();
	setup_node_info();
	setup_paths();
	save_hugepage_configs();
	wake_cond = 0;

	test1();
	test2();
	test3();
	test4();
	test5();

	restore_hugepage_configs();
	ksft_finished();
}
